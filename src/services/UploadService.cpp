#include "services/UploadService.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "common/Hash.h"
#include "common/Logging.h"
#include "domain/Role.h"
#include "domain/Validation.h"

namespace launcher::services {
namespace {

using common::ErrorCode;
using common::Result;
using common::VoidResult;
using domain::Actor;
using repositories::UploadSession;
using repositories::UploadSessionStatus;

/// Build ids are opaque, so a caller who does not own one is told it does not exist rather
/// than that it exists and is out of reach.
constexpr const char* NO_SUCH_BUILD = "no such build";

constexpr const char* NO_SUCH_SESSION = "no such upload session";

/// How many expired sessions one sweep reclaims. Bounded so the timer never turns into a long
/// blocking scan on a server with a bad day behind it.
constexpr int SWEEP_BATCH = 100;

/// Missing blobs reported back on a rejected manifest. Enough to be actionable, few enough
/// that a wildly wrong request does not produce a megabyte of error detail.
constexpr std::size_t MAX_REPORTED_MISSING = 20;

bool mayPublish(const domain::BuildOwnership& ownership, const Actor& actor) {
    return actor.owns(ownership.publisherUserId) || actor.managesAnyGame();
}

} // namespace

UploadService::UploadService(const repositories::IBuildRepository& builds,
                             const repositories::IBlobRepository& blobs,
                             const repositories::IUploadSessionRepository& sessions,
                             const repositories::IUserRepository& users,
                             storage::BlobStore blobStore,
                             UploadSettings settings)
    : builds_(builds),
      blobs_(blobs),
      sessions_(sessions),
      users_(users),
      blobStore_(std::move(blobStore)),
      settings_(std::move(settings)) {}

drogon::Task<Result<domain::BuildOwnership>>
UploadService::writableBuild(Actor actor, std::string buildId) const {
    if (!actor.can(domain::permissions::BUILD_UPLOAD)) {
        co_return Result<domain::BuildOwnership>::failure(ErrorCode::Forbidden,
                                                          "you cannot upload builds");
    }
    if (!domain::isUuid(buildId)) {
        co_return Result<domain::BuildOwnership>::failure(ErrorCode::NotFound, NO_SUCH_BUILD);
    }

    const auto ownership = co_await builds_.findOwnership(buildId);
    if (!ownership.has_value() || !mayPublish(*ownership, actor)) {
        co_return Result<domain::BuildOwnership>::failure(ErrorCode::NotFound, NO_SUCH_BUILD);
    }
    if (ownership->status != domain::BuildStatus::Uploading) {
        co_return Result<domain::BuildOwnership>::failure(ErrorCode::Conflict,
                                                          "this build has already been finalized");
    }

    co_return Result<domain::BuildOwnership>::success(*ownership);
}

drogon::Task<Result<std::vector<std::string>>> UploadService::missingBlobs(
    Actor actor, std::string buildId, std::vector<BlobDeclaration> blobs) const {
    auto build = co_await writableBuild(actor, buildId);
    if (!build.ok()) {
        co_return Result<std::vector<std::string>>::failure(build.error());
    }

    if (blobs.empty()) {
        co_return Result<std::vector<std::string>>::success({});
    }
    if (blobs.size() > settings_.maxNegotiationBatch) {
        co_return Result<std::vector<std::string>>::failure(
            ErrorCode::InvalidInput,
            "ask about at most " + std::to_string(settings_.maxNegotiationBatch) +
                " blobs per request");
    }

    std::vector<std::string> candidates;
    std::unordered_set<std::string> seen;
    candidates.reserve(blobs.size());

    for (const auto& blob : blobs) {
        if (!domain::isSha256Hex(blob.sha256)) {
            co_return Result<std::vector<std::string>>::failure(
                ErrorCode::InvalidInput, "sha256 must be 64 lowercase hex characters");
        }
        if (blob.sizeBytes < 0 || blob.sizeBytes > settings_.maxBlobBytes) {
            co_return Result<std::vector<std::string>>::failure(
                ErrorCode::InvalidInput,
                "a single file may be at most " + std::to_string(settings_.maxBlobBytes) +
                    " bytes");
        }
        if (seen.insert(blob.sha256).second) {
            candidates.push_back(blob.sha256);
        }
    }

    co_return Result<std::vector<std::string>>::success(
        co_await blobs_.findMissing(std::move(candidates)));
}

drogon::Task<Result<UploadSession>>
UploadService::beginUpload(Actor actor, std::string buildId, BlobDeclaration blob) const {
    auto build = co_await writableBuild(actor, buildId);
    if (!build.ok()) {
        co_return Result<UploadSession>::failure(build.error());
    }

    if (!domain::isSha256Hex(blob.sha256)) {
        co_return Result<UploadSession>::failure(ErrorCode::InvalidInput,
                                                 "sha256 must be 64 lowercase hex characters");
    }
    if (blob.sizeBytes < 0 || blob.sizeBytes > settings_.maxBlobBytes) {
        co_return Result<UploadSession>::failure(
            ErrorCode::InvalidInput,
            "a single file may be at most " + std::to_string(settings_.maxBlobBytes) + " bytes");
    }

    // Reported rather than silently accepted: the client is meant to negotiate first, and
    // knowing the blob is already there is exactly the answer it needs.
    if (co_await blobs_.findBySha256(blob.sha256)) {
        co_return Result<UploadSession>::failure(ErrorCode::Conflict,
                                                 "the server already holds this content");
    }

    if (co_await sessions_.countOpenForUser(actor.userId) >= settings_.maxOpenSessionsPerUser) {
        co_return Result<UploadSession>::failure(
            ErrorCode::Conflict,
            "you have too many uploads in flight; finish or abort one before starting another");
    }

    // An advisory check only. The binding one happens when the upload completes, where the
    // charge is a single conditional statement that concurrent uploads cannot both win.
    const auto user = co_await users_.findById(actor.userId);
    if (!user.has_value()) {
        co_return Result<UploadSession>::failure(ErrorCode::Unauthenticated, "no such account");
    }
    if (user->uploadUsedBytes + blob.sizeBytes > user->uploadQuotaBytes) {
        co_return Result<UploadSession>::failure(ErrorCode::QuotaExceeded,
                                                 "this upload would take you past your quota of " +
                                                     std::to_string(user->uploadQuotaBytes) +
                                                     " bytes");
    }

    repositories::NewUploadSession request;
    request.buildId = build.value().buildId;
    request.userId = actor.userId;
    request.blobSha256 = blob.sha256;
    request.declaredSizeBytes = blob.sizeBytes;
    request.ttl = settings_.sessionTtl;

    auto session = co_await sessions_.create(std::move(request));
    if (session.ok() && session.value().userId != actor.userId) {
        co_return Result<UploadSession>::failure(ErrorCode::Conflict,
                                                 "another session is already uploading this file");
    }
    co_return session;
}

drogon::Task<Result<UploadSession>> UploadService::sessionStatus(Actor actor,
                                                                 std::string sessionId) const {
    if (!domain::isUuid(sessionId)) {
        co_return Result<UploadSession>::failure(ErrorCode::NotFound, NO_SUCH_SESSION);
    }

    const auto session = co_await sessions_.findById(sessionId);
    if (!session.has_value() || session->userId != actor.userId) {
        co_return Result<UploadSession>::failure(ErrorCode::NotFound, NO_SUCH_SESSION);
    }
    co_return Result<UploadSession>::success(*session);
}

drogon::Task<Result<UploadSession>> UploadService::uploadChunk(Actor actor,
                                                               std::string sessionId,
                                                               int64_t offset,
                                                               std::string chunk) const {
    auto found = co_await sessionStatus(actor, sessionId);
    if (!found.ok()) {
        co_return found;
    }

    const auto session = std::move(found).value();
    if (session.status != UploadSessionStatus::Pending) {
        co_return Result<UploadSession>::failure(ErrorCode::Conflict,
                                                 "this upload has already finished");
    }
    if (session.expired) {
        co_return Result<UploadSession>::failure(
            ErrorCode::Conflict, "this upload expired; start it again from the beginning");
    }

    const auto length = static_cast<int64_t>(chunk.size());
    if (length > settings_.maxChunkBytes) {
        co_return Result<UploadSession>::failure(
            ErrorCode::InvalidInput,
            "a chunk may be at most " + std::to_string(settings_.maxChunkBytes) + " bytes");
    }
    if (offset != session.receivedBytes) {
        // The session's own offset travels with the error, so a client that lost track of how
        // far it got resumes from this response instead of starting over.
        co_return Result<UploadSession>::failure(ErrorCode::Conflict,
                                                 "this upload is at offset " +
                                                     std::to_string(session.receivedBytes) +
                                                     ", not " + std::to_string(offset));
    }
    if (offset + length > session.declaredSizeBytes) {
        co_return Result<UploadSession>::failure(ErrorCode::InvalidInput,
                                                 "this chunk runs past the declared file size");
    }

    // Reserve first, write second. The database decides who owns this range, so two chunks
    // racing at the same offset cannot both be told to write it.
    const auto reserved = co_await sessions_.reserveBytes(session.id, offset, length);
    if (!reserved.has_value()) {
        co_return Result<UploadSession>::failure(
            ErrorCode::Conflict,
            "another request is writing to this upload; retry after a status "
            "check");
    }

    const auto staging = blobStore_.stagingPathFor(session.id);
    if (auto written = blobStore_.writeAt(staging, offset, chunk); !written.ok()) {
        // The database now says more bytes arrived than the file holds, so the session cannot
        // be resumed and is closed rather than left to fail its hash check much later.
        co_await sessions_.markAborted(session.id);
        blobStore_.discard(staging);
        co_return Result<UploadSession>::failure(written.error());
    }

    if (!reserved->complete()) {
        co_return Result<UploadSession>::success(*reserved);
    }

    if (auto completed = co_await completeSession(*reserved); !completed.ok()) {
        co_return Result<UploadSession>::failure(completed.error());
    }

    auto finished = *reserved;
    finished.status = UploadSessionStatus::Completed;
    co_return Result<UploadSession>::success(std::move(finished));
}

drogon::Task<VoidResult> UploadService::completeSession(UploadSession session) const {
    const auto staging = blobStore_.stagingPathFor(session.id);

    // Somebody else stored this content while the upload was in flight. Nothing new lands, so
    // nothing is charged.
    if (co_await blobs_.findBySha256(session.blobSha256)) {
        blobStore_.discard(staging);
        co_await sessions_.markCompleted(session.id);
        co_return VoidResult::success();
    }

    if (!co_await users_.chargeUpload(session.userId, session.declaredSizeBytes)) {
        blobStore_.discard(staging);
        co_await sessions_.markAborted(session.id);
        co_return VoidResult::failure(ErrorCode::QuotaExceeded,
                                      "storing this file would take you past your upload quota");
    }

    if (auto committed = blobStore_.commit(staging, session.blobSha256); !committed.ok()) {
        co_await users_.releaseUpload(session.userId, session.declaredSizeBytes);
        co_await sessions_.markAborted(session.id);
        spdlog::warn("discarded upload sessionId={} sha256={} reason={}",
                     common::escapeJson(session.id),
                     common::escapeJson(session.blobSha256),
                     common::escapeJson(committed.error().detail));
        co_return committed;
    }

    const auto stored =
        co_await blobs_.record(session.blobSha256,
                               session.declaredSizeBytes,
                               storage::BlobStore::storageKeyFor(session.blobSha256),
                               session.userId);
    if (!stored) {
        // Lost the race to record the row. The bytes are identical either way, so the only
        // thing to undo is the charge.
        co_await users_.releaseUpload(session.userId, session.declaredSizeBytes);
    }

    co_await sessions_.markCompleted(session.id);
    co_return VoidResult::success();
}

drogon::Task<VoidResult> UploadService::abortUpload(Actor actor, std::string sessionId) const {
    const auto found = co_await sessionStatus(actor, sessionId);
    if (!found.ok()) {
        co_return VoidResult::failure(found.error());
    }

    co_await sessions_.markAborted(found.value().id);
    blobStore_.discard(blobStore_.stagingPathFor(found.value().id));
    co_return VoidResult::success();
}

drogon::Task<Result<domain::Build>>
UploadService::finalizeBuild(Actor actor, std::string buildId, FinalizeBuildCommand command) const {
    auto build = co_await writableBuild(actor, buildId);
    if (!build.ok()) {
        co_return Result<domain::Build>::failure(build.error());
    }

    const auto entrypoint = domain::trim(command.entrypointRelativePath);
    if (command.defaultLaunchArgs.size() > domain::MAX_LAUNCH_ARGS_LENGTH) {
        co_return Result<domain::Build>::failure(
            ErrorCode::InvalidInput,
            "launchArgs must be at most " + std::to_string(domain::MAX_LAUNCH_ARGS_LENGTH) +
                " characters");
    }
    if (auto check = domain::validateManifest(command.files, entrypoint); !check.ok()) {
        co_return Result<domain::Build>::failure(check.error());
    }

    std::vector<std::string> distinct;
    std::unordered_set<std::string> seen;
    for (const auto& file : command.files) {
        if (seen.insert(file.blobSha256).second) {
            distinct.push_back(file.blobSha256);
        }
    }

    const auto records = co_await blobs_.findMany(distinct);
    std::unordered_map<std::string, int64_t> sizes;
    sizes.reserve(records.size());
    for (const auto& record : records) {
        sizes.emplace(record.sha256, record.sizeBytes);
    }

    if (sizes.size() != distinct.size()) {
        std::string detail = "upload these blobs before finalizing the build:";
        std::size_t reported = 0;
        for (const auto& sha256 : distinct) {
            if (sizes.find(sha256) != sizes.end()) {
                continue;
            }
            if (reported++ == MAX_REPORTED_MISSING) {
                detail += " and more";
                break;
            }
            detail += " " + sha256;
        }
        co_return Result<domain::Build>::failure(ErrorCode::InvalidInput, std::move(detail));
    }

    // Sizes come from the store, never from the request: a manifest must not be able to claim
    // a download size its blobs do not have.
    repositories::FinalizedManifest manifest;
    manifest.files.reserve(command.files.size());
    for (const auto& file : command.files) {
        auto entry = file;
        entry.sizeBytes = sizes.at(file.blobSha256);
        manifest.totalSizeBytes += entry.sizeBytes;
        manifest.files.push_back(std::move(entry));
    }

    manifest.entrypointRelativePath = entrypoint;
    manifest.defaultLaunchArgs = command.defaultLaunchArgs;

    const auto document = domain::canonicalManifestDocument(
        manifest.files, manifest.entrypointRelativePath, manifest.defaultLaunchArgs);
    manifest.manifestSha256 = common::sha256Hex(document);

    auto finalized = co_await builds_.finalize(build.value().buildId, std::move(manifest));
    if (!finalized.has_value()) {
        co_return Result<domain::Build>::failure(ErrorCode::Conflict,
                                                 "this build has already been finalized");
    }

    spdlog::info("finalized build id={} files={} bytes={}",
                 common::escapeJson(finalized->id),
                 finalized->fileCount,
                 finalized->totalSizeBytes);

    co_return Result<domain::Build>::success(std::move(*finalized));
}

drogon::Task<Result<ManifestDocument>> UploadService::manifest(Actor actor,
                                                               std::string buildId) const {
    if (!actor.can(domain::permissions::GAME_DOWNLOAD)) {
        co_return Result<ManifestDocument>::failure(ErrorCode::Forbidden,
                                                    "you cannot download builds");
    }
    if (!domain::isUuid(buildId)) {
        co_return Result<ManifestDocument>::failure(ErrorCode::NotFound, NO_SUCH_BUILD);
    }

    const auto ownership = co_await builds_.findOwnership(buildId);
    if (!ownership.has_value()) {
        co_return Result<ManifestDocument>::failure(ErrorCode::NotFound, NO_SUCH_BUILD);
    }

    const bool draft = ownership->visibility == domain::GameVisibility::Draft;
    if (draft && !mayPublish(*ownership, actor)) {
        co_return Result<ManifestDocument>::failure(ErrorCode::NotFound, NO_SUCH_BUILD);
    }
    if (ownership->status != domain::BuildStatus::Ready) {
        co_return Result<ManifestDocument>::failure(ErrorCode::NotFound,
                                                    "this build is not downloadable yet");
    }

    const auto build = co_await builds_.findById(buildId);
    if (!build.has_value()) {
        co_return Result<ManifestDocument>::failure(ErrorCode::NotFound, NO_SUCH_BUILD);
    }

    const auto files = co_await builds_.filesFor(buildId);

    ManifestDocument document;
    document.sha256 = build->manifestSha256;
    document.json = domain::canonicalManifestDocument(
        files, build->entrypointRelativePath, build->defaultLaunchArgs);
    co_return Result<ManifestDocument>::success(std::move(document));
}

drogon::Task<std::size_t> UploadService::sweepExpiredSessions() const {
    const auto expired = co_await sessions_.findExpired(SWEEP_BATCH);

    std::size_t reclaimed = 0;
    for (const auto& session : expired) {
        blobStore_.discard(blobStore_.stagingPathFor(session.id));
        if (co_await sessions_.deleteById(session.id)) {
            ++reclaimed;
        }
    }

    if (reclaimed > 0) {
        spdlog::info("reclaimed {} abandoned upload sessions", reclaimed);
    }
    co_return reclaimed;
}

} // namespace launcher::services
