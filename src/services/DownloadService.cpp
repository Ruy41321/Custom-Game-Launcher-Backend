#include "services/DownloadService.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "common/Logging.h"
#include "domain/Role.h"
#include "domain/Validation.h"
#include "storage/BlobStore.h"

namespace launcher::services {
namespace {

using common::ErrorCode;
using common::Result;
using domain::Actor;

/// Build ids are opaque, so a build the caller may not see is reported missing rather than
/// forbidden: a 403 would confirm that it exists.
constexpr const char* NO_SUCH_BUILD = "no such build";

int64_t distinctBlobBytes(const std::vector<DownloadFile>& files) {
    std::unordered_set<std::string> counted;
    int64_t bytes = 0;
    for (const auto& file : files) {
        if (counted.insert(file.entry.blobSha256).second) {
            bytes += file.entry.sizeBytes;
        }
    }
    return bytes;
}

} // namespace

DownloadService::DownloadService(const repositories::IBuildRepository& builds,
                                 const repositories::IDownloadRepository& downloads,
                                 storage::DownloadUrlSigner signer,
                                 DownloadSettings settings)
    : builds_(builds),
      downloads_(downloads),
      signer_(std::move(signer)),
      settings_(std::move(settings)) {}

drogon::Task<Result<domain::BuildOwnership>>
DownloadService::downloadableBuild(Actor actor, std::string buildId) const {
    if (!actor.can(domain::permissions::GAME_DOWNLOAD)) {
        co_return Result<domain::BuildOwnership>::failure(ErrorCode::Forbidden,
                                                          "you cannot download builds");
    }
    if (!domain::isUuid(buildId)) {
        co_return Result<domain::BuildOwnership>::failure(ErrorCode::NotFound, NO_SUCH_BUILD);
    }

    const auto ownership = co_await builds_.findOwnership(buildId);
    if (!ownership.has_value() || !domain::mayReadBuild(*ownership, actor)) {
        co_return Result<domain::BuildOwnership>::failure(ErrorCode::NotFound, NO_SUCH_BUILD);
    }
    if (ownership->status != domain::BuildStatus::Ready) {
        // Not a conflict: a build still uploading has no manifest, so from the outside there is
        // nothing there to download yet.
        co_return Result<domain::BuildOwnership>::failure(ErrorCode::NotFound,
                                                          "this build is not downloadable yet");
    }

    co_return Result<domain::BuildOwnership>::success(*ownership);
}

std::vector<DownloadFile> DownloadService::signAll(const std::vector<domain::DeltaFile>& files,
                                                   int64_t expiresAt) const {
    std::vector<DownloadFile> signed_;
    signed_.reserve(files.size());

    // One URL per blob, reused across the paths that share it: the same content is one
    // download however many places it lands in.
    std::unordered_map<std::string, std::string> urls;
    for (const auto& file : files) {
        auto url = urls.find(file.entry.blobSha256);
        if (url == urls.end()) {
            url =
                urls.emplace(file.entry.blobSha256,
                             signer_.sign(storage::BlobStore::storageKeyFor(file.entry.blobSha256),
                                          expiresAt))
                    .first;
        }
        signed_.push_back(DownloadFile{file.entry, file.copyFromRelativePath, url->second});
    }
    return signed_;
}

drogon::Task<Result<DownloadPlan>>
DownloadService::plan(Actor actor, std::string buildId, std::string fromBuildId) const {
    auto target = co_await downloadableBuild(actor, buildId);
    if (!target.ok()) {
        co_return Result<DownloadPlan>::failure(target.error());
    }

    std::vector<domain::ManifestEntry> installed;
    std::string fromVersionId;
    if (!fromBuildId.empty()) {
        auto source = co_await downloadableBuild(actor, fromBuildId);
        // A source is a claim about what is already on the caller's disk, not something being
        // served: nothing about it reaches the answer except which bytes may be skipped. So a
        // source this caller may no longer read — the publisher withdrew that version, or
        // deleted it — costs a full download of the target rather than a refusal. Refusing
        // would leave a player who installed a version that was later withdrawn unable to
        // update at all, which is a larger consequence than the one the withdrawal chose.
        if (!source.ok()) {
            spdlog::info("planning a full download: the source build is not readable buildId={}",
                         common::escapeJson(fromBuildId));
        } else if (source.value().gameId != target.value().gameId) {
            co_return Result<DownloadPlan>::failure(
                ErrorCode::InvalidInput,
                "the build you are updating from belongs to a different game");
        } else {
            fromVersionId = source.value().gameVersionId;
            installed = co_await builds_.filesFor(fromBuildId);
        }
    }

    const auto build = co_await builds_.findById(buildId);
    if (!build.has_value()) {
        co_return Result<DownloadPlan>::failure(ErrorCode::NotFound, NO_SUCH_BUILD);
    }
    const auto files = co_await builds_.filesFor(buildId);

    auto delta = installed.empty() ? domain::fullDownload(installed, files)
                                   : domain::computeDelta(installed, files);

    // Past the threshold a delta costs nearly as much as the build itself, and a full download
    // is the simpler thing to get right on the client. The comparison is a multiplication so a
    // build of zero-byte files cannot divide by zero.
    if (delta.kind == domain::DownloadKind::Delta &&
        static_cast<double>(delta.downloadBytes) >
            settings_.fullDownloadThresholdRatio * static_cast<double>(delta.totalBytes)) {
        delta = domain::fullDownload(installed, files);
    }

    const auto expiresAt = signer_.expiryFor(std::chrono::system_clock::now());

    DownloadPlan plan;
    plan.buildId = target.value().buildId;
    plan.gameId = target.value().gameId;
    plan.gameVersionId = target.value().gameVersionId;
    plan.kind = delta.kind;
    plan.manifestSha256 = build->manifestSha256;
    plan.entrypointRelativePath = build->entrypointRelativePath;
    plan.defaultLaunchArgs = build->defaultLaunchArgs;
    plan.files = signAll(delta.fetch, expiresAt);
    plan.unchanged = std::move(delta.unchanged);
    plan.remove = std::move(delta.remove);
    plan.downloadBytes = delta.downloadBytes;
    plan.totalBytes = delta.totalBytes;
    plan.urlsExpireAt = expiresAt;

    // A plan with nothing to fetch is a client confirming it is already up to date, not a
    // download, and counting it would make the statistics say the opposite of the truth.
    if (!plan.files.empty()) {
        repositories::DownloadEvent event;
        event.gameId = plan.gameId;
        event.buildId = plan.buildId;
        event.userId = actor.userId;
        event.fromVersionId = fromVersionId;
        event.kind = plan.kind;
        event.bytesPlanned = plan.downloadBytes;
        co_await downloads_.recordEvent(std::move(event));
    }

    spdlog::info("planned {} download buildId={} files={} bytes={}",
                 domain::toString(plan.kind),
                 common::escapeJson(plan.buildId),
                 plan.files.size(),
                 plan.downloadBytes);

    co_return Result<DownloadPlan>::success(std::move(plan));
}

drogon::Task<Result<IntegrityReport>> DownloadService::verifyInstall(
    Actor actor, std::string buildId, std::vector<InstalledFile> installed) const {
    auto ownership = co_await downloadableBuild(actor, buildId);
    if (!ownership.ok()) {
        co_return Result<IntegrityReport>::failure(ownership.error());
    }

    if (installed.size() > domain::MAX_MANIFEST_ENTRIES) {
        co_return Result<IntegrityReport>::failure(
            ErrorCode::InvalidInput,
            "an install may contain at most " + std::to_string(domain::MAX_MANIFEST_ENTRIES) +
                " files");
    }

    std::unordered_map<std::string, std::string> observed;
    observed.reserve(installed.size());
    for (const auto& file : installed) {
        if (file.relativePath.empty() ||
            file.relativePath.size() > domain::MAX_RELATIVE_PATH_LENGTH) {
            co_return Result<IntegrityReport>::failure(
                ErrorCode::InvalidInput,
                "each reported file needs a path of at most " +
                    std::to_string(domain::MAX_RELATIVE_PATH_LENGTH) + " characters");
        }
        // A file the client could not read is reported by leaving it out, where it lands among
        // the missing ones. Sending a hash that is not one is a bug worth surfacing.
        if (!domain::isSha256Hex(file.sha256)) {
            co_return Result<IntegrityReport>::failure(
                ErrorCode::InvalidInput,
                "sha256 must be 64 lowercase hex characters, for " + file.relativePath);
        }
        if (!observed.emplace(file.relativePath, file.sha256).second) {
            co_return Result<IntegrityReport>::failure(
                ErrorCode::Conflict, "you reported " + file.relativePath + " twice");
        }
    }

    const auto build = co_await builds_.findById(buildId);
    if (!build.has_value()) {
        co_return Result<IntegrityReport>::failure(ErrorCode::NotFound, NO_SUCH_BUILD);
    }
    const auto expected = co_await builds_.filesFor(buildId);

    IntegrityReport report;
    report.buildId = ownership.value().buildId;
    report.manifestSha256 = build->manifestSha256;

    std::vector<domain::DeltaFile> damaged;
    for (const auto& entry : expected) {
        const auto found = observed.find(entry.relativePath);
        if (found == observed.end()) {
            report.missing.push_back(entry.relativePath);
        } else if (found->second != entry.blobSha256) {
            report.corrupt.push_back(entry.relativePath);
        } else {
            continue;
        }
        damaged.push_back(domain::DeltaFile{entry, {}});
    }

    std::unordered_set<std::string> manifestPaths;
    manifestPaths.reserve(expected.size());
    for (const auto& entry : expected) {
        manifestPaths.insert(entry.relativePath);
    }
    for (const auto& file : installed) {
        if (manifestPaths.find(file.relativePath) == manifestPaths.end()) {
            report.unexpected.push_back(file.relativePath);
        }
    }
    std::sort(report.unexpected.begin(), report.unexpected.end());

    report.urlsExpireAt = signer_.expiryFor(std::chrono::system_clock::now());
    report.repair = signAll(damaged, report.urlsExpireAt);
    report.repairBytes = distinctBlobBytes(report.repair);
    // Files the manifest never mentioned do not make an install broken: an install directory
    // legitimately accumulates saves, configuration and logs. They are reported so the client
    // can decide, not so the server can order a deletion.
    report.intact = report.missing.empty() && report.corrupt.empty();

    co_return Result<IntegrityReport>::success(std::move(report));
}

} // namespace launcher::services
