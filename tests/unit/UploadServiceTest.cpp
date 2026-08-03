#include <gtest/gtest.h>

#include <drogon/utils/coroutine.h>

#include <filesystem>
#include <string>
#include <vector>

#include "common/Hash.h"
#include "common/Random.h"
#include "domain/Role.h"
#include "services/UploadService.h"
#include "support/FakeCatalogRepositories.h"
#include "support/FakeRepositories.h"
#include "support/FakeUploadRepositories.h"
#include "support/TemporaryDirectory.h"

namespace {

using launcher::common::ErrorCode;
using launcher::common::sha256Hex;
using launcher::domain::Actor;
using launcher::domain::Build;
using launcher::domain::BuildOwnership;
using launcher::domain::BuildStatus;
using launcher::domain::GameVisibility;
using launcher::domain::ManifestEntry;
using launcher::services::BlobDeclaration;
using launcher::services::FinalizeBuildCommand;
using launcher::services::UploadService;
using launcher::services::UploadSettings;
using launcher::testing::FakeBlobRepository;
using launcher::testing::FakeBuildRepository;
using launcher::testing::FakeUploadSessionRepository;
using launcher::testing::FakeUserRepository;
using launcher::testing::TemporaryDirectory;

namespace permissions = launcher::domain::permissions;

const std::string PUBLISHER = launcher::common::randomUuid();

Actor publisher(std::string userId = PUBLISHER) {
    return Actor{std::move(userId),
                 {permissions::GAME_READ,
                  permissions::GAME_DOWNLOAD,
                  permissions::GAME_PUBLISH,
                  permissions::BUILD_UPLOAD}};
}

/// Wires the service over fresh fakes and a throwaway blob root, and seeds one build that is
/// still uploading. Everything is owned by the fixture so the service's references stay valid.
struct UploadFixture {
    explicit UploadFixture(UploadSettings settings = UploadSettings{})
        : service(builds,
                  blobs,
                  sessions,
                  users,
                  launcher::storage::BlobStore{root.path()},
                  std::move(settings)) {
        launcher::domain::User user;
        user.id = PUBLISHER;
        user.email = "dev@example.com";
        user.displayName = "Dev";
        user.uploadQuotaBytes = 1024 * 1024;
        users.seed(user);

        Build build;
        build.gameVersionId = launcher::common::randomUuid();
        build.status = BuildStatus::Uploading;

        BuildOwnership ownership;
        ownership.gameId = launcher::common::randomUuid();
        ownership.publisherUserId = PUBLISHER;
        ownership.visibility = GameVisibility::Public;

        buildId = builds.seed(build, ownership).id;
    }

    TemporaryDirectory root;
    FakeBuildRepository builds;
    FakeBlobRepository blobs;
    FakeUploadSessionRepository sessions;
    FakeUserRepository users;
    UploadService service;
    std::string buildId;

    /// Runs a whole upload of `content` and returns the session as the service last saw it.
    launcher::common::Result<launcher::repositories::UploadSession>
    upload(const std::string& content) {
        auto session = drogon::sync_wait(service.beginUpload(
            publisher(),
            buildId,
            BlobDeclaration{sha256Hex(content), static_cast<int64_t>(content.size())}));
        if (!session.ok()) {
            return session;
        }
        return drogon::sync_wait(service.uploadChunk(publisher(), session.value().id, 0, content));
    }

    int64_t usedBytes() const { return users.users.front().uploadUsedBytes; }
};

FinalizeBuildCommand manifestOf(const std::vector<ManifestEntry>& files,
                                const std::string& entrypoint) {
    FinalizeBuildCommand command;
    command.files = files;
    command.entrypointRelativePath = entrypoint;
    return command;
}

// ---------------------------------------------------------------------------
// Negotiation
// ---------------------------------------------------------------------------

// This is what keeps an update proportional to what changed: the client only sends what the
// server has never seen.
TEST(UploadServiceTest, ReportsOnlyTheBlobsTheServerDoesNotHave) {
    UploadFixture fixture;
    const auto known = sha256Hex("already stored");
    const auto fresh = sha256Hex("brand new");
    fixture.blobs.seed(known, 14);

    const auto missing = drogon::sync_wait(fixture.service.missingBlobs(
        publisher(), fixture.buildId, {BlobDeclaration{known, 14}, BlobDeclaration{fresh, 9}}));

    ASSERT_TRUE(missing.ok()) << missing.error().detail;
    ASSERT_EQ(missing.value().size(), 1U);
    EXPECT_EQ(missing.value()[0], fresh);
}

TEST(UploadServiceTest, RefusesNegotiationForABuildTheActorDoesNotOwn) {
    UploadFixture fixture;

    const auto missing = drogon::sync_wait(fixture.service.missingBlobs(
        publisher(launcher::common::randomUuid()), fixture.buildId, {}));

    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound) << "an unowned build must not be "
                                                            "distinguishable from a missing one";
}

TEST(UploadServiceTest, RejectsAMalformedContentAddress) {
    UploadFixture fixture;

    const auto missing = drogon::sync_wait(fixture.service.missingBlobs(
        publisher(), fixture.buildId, {BlobDeclaration{"not-a-hash", 1}}));

    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.error().code, ErrorCode::InvalidInput);
}

// ---------------------------------------------------------------------------
// Transfer
// ---------------------------------------------------------------------------

TEST(UploadServiceTest, StoresABlobOnceItsBytesHashToTheDeclaredAddress) {
    UploadFixture fixture;
    const std::string content = "a small game file";

    const auto session = fixture.upload(content);

    ASSERT_TRUE(session.ok()) << session.error().detail;
    EXPECT_TRUE(session.value().complete());
    EXPECT_EQ(drogon::sync_wait(fixture.blobs.findBySha256(sha256Hex(content))).has_value(), true);
    EXPECT_EQ(fixture.usedBytes(), static_cast<int64_t>(content.size()));
}

TEST(UploadServiceTest, ResumesFromTheOffsetTheServerRecorded) {
    UploadFixture fixture;
    const std::string content = "resume me halfway through";
    const auto digest = sha256Hex(content);

    auto session = drogon::sync_wait(
        fixture.service.beginUpload(publisher(),
                                    fixture.buildId,
                                    BlobDeclaration{digest, static_cast<int64_t>(content.size())}));
    ASSERT_TRUE(session.ok()) << session.error().detail;
    const auto id = session.value().id;

    const auto first =
        drogon::sync_wait(fixture.service.uploadChunk(publisher(), id, 0, content.substr(0, 10)));
    ASSERT_TRUE(first.ok()) << first.error().detail;
    EXPECT_EQ(first.value().receivedBytes, 10);
    EXPECT_FALSE(first.value().complete());

    // The client comes back after an interruption and asks where it got to.
    const auto status = drogon::sync_wait(fixture.service.sessionStatus(publisher(), id));
    ASSERT_TRUE(status.ok());
    ASSERT_EQ(status.value().receivedBytes, 10);

    const auto second = drogon::sync_wait(fixture.service.uploadChunk(
        publisher(), id, status.value().receivedBytes, content.substr(10)));

    ASSERT_TRUE(second.ok()) << second.error().detail;
    EXPECT_TRUE(second.value().complete());
    EXPECT_TRUE(drogon::sync_wait(fixture.blobs.findBySha256(digest)).has_value());
}

// A chunk written at the wrong place would corrupt the file in a way only the final hash
// would catch, after the whole upload had been paid for.
TEST(UploadServiceTest, RefusesAChunkAtTheWrongOffsetAndSaysWhereToResume) {
    UploadFixture fixture;
    const std::string content = "0123456789";

    auto session = drogon::sync_wait(fixture.service.beginUpload(
        publisher(), fixture.buildId, BlobDeclaration{sha256Hex(content), 10}));
    ASSERT_TRUE(session.ok());
    ASSERT_TRUE(
        drogon::sync_wait(fixture.service.uploadChunk(publisher(), session.value().id, 0, "01234"))
            .ok());

    const auto wrong =
        drogon::sync_wait(fixture.service.uploadChunk(publisher(), session.value().id, 0, "56789"));

    ASSERT_FALSE(wrong.ok());
    EXPECT_EQ(wrong.error().code, ErrorCode::Conflict);
    EXPECT_NE(wrong.error().detail.find("offset 5"), std::string::npos)
        << "the error must carry the offset to resume from: " << wrong.error().detail;
}

TEST(UploadServiceTest, RefusesAChunkThatRunsPastTheDeclaredSize) {
    UploadFixture fixture;

    auto session = drogon::sync_wait(fixture.service.beginUpload(
        publisher(), fixture.buildId, BlobDeclaration{sha256Hex("tiny"), 4}));
    ASSERT_TRUE(session.ok());

    const auto oversized = drogon::sync_wait(
        fixture.service.uploadChunk(publisher(), session.value().id, 0, "far too many bytes"));

    ASSERT_FALSE(oversized.ok());
    EXPECT_EQ(oversized.error().code, ErrorCode::InvalidInput);
}

// The defence against a corrupted transfer or a tampered blob: nothing that fails its own
// hash is ever stored, and the account is not billed for it.
TEST(UploadServiceTest, DiscardsAnUploadWhoseBytesDoNotMatchAndRefundsTheQuota) {
    UploadFixture fixture;
    const auto claimed = sha256Hex("what was promised");
    const std::string sent = "what was actually sent";

    auto session = drogon::sync_wait(fixture.service.beginUpload(
        publisher(), fixture.buildId, BlobDeclaration{claimed, static_cast<int64_t>(sent.size())}));
    ASSERT_TRUE(session.ok());

    const auto uploaded =
        drogon::sync_wait(fixture.service.uploadChunk(publisher(), session.value().id, 0, sent));

    ASSERT_FALSE(uploaded.ok());
    EXPECT_EQ(uploaded.error().code, ErrorCode::InvalidInput);
    EXPECT_FALSE(drogon::sync_wait(fixture.blobs.findBySha256(claimed)).has_value());
    EXPECT_EQ(fixture.usedBytes(), 0) << "a rejected upload must not consume quota";
}

TEST(UploadServiceTest, RefusesToStartAnUploadForContentTheServerAlreadyHas) {
    UploadFixture fixture;
    const auto digest = sha256Hex("shared");
    fixture.blobs.seed(digest, 6);

    const auto session = drogon::sync_wait(
        fixture.service.beginUpload(publisher(), fixture.buildId, BlobDeclaration{digest, 6}));

    ASSERT_FALSE(session.ok());
    EXPECT_EQ(session.error().code, ErrorCode::Conflict);
}

TEST(UploadServiceTest, RefusesToTouchAnotherAccountsUploadSession) {
    UploadFixture fixture;
    auto session = drogon::sync_wait(fixture.service.beginUpload(
        publisher(), fixture.buildId, BlobDeclaration{sha256Hex("mine"), 4}));
    ASSERT_TRUE(session.ok());

    const auto stranger = drogon::sync_wait(fixture.service.sessionStatus(
        publisher(launcher::common::randomUuid()), session.value().id));

    ASSERT_FALSE(stranger.ok());
    EXPECT_EQ(stranger.error().code, ErrorCode::NotFound);
}

TEST(UploadServiceTest, RefusesAChunkForAnExpiredSession) {
    UploadFixture fixture;
    auto session = drogon::sync_wait(fixture.service.beginUpload(
        publisher(), fixture.buildId, BlobDeclaration{sha256Hex("late"), 4}));
    ASSERT_TRUE(session.ok());
    fixture.sessions.expiredIds.insert(session.value().id);

    const auto uploaded =
        drogon::sync_wait(fixture.service.uploadChunk(publisher(), session.value().id, 0, "late"));

    ASSERT_FALSE(uploaded.ok());
    EXPECT_EQ(uploaded.error().code, ErrorCode::Conflict);
}

TEST(UploadServiceTest, AbortingAnUploadRemovesItsStagingFile) {
    UploadFixture fixture;
    const std::string content = "abandon me";
    auto session = drogon::sync_wait(fixture.service.beginUpload(
        publisher(),
        fixture.buildId,
        BlobDeclaration{sha256Hex(content), static_cast<int64_t>(content.size() + 10)}));
    ASSERT_TRUE(session.ok());
    ASSERT_TRUE(
        drogon::sync_wait(fixture.service.uploadChunk(publisher(), session.value().id, 0, content))
            .ok());

    const launcher::storage::BlobStore store(fixture.root.path());
    const auto staging = store.stagingPathFor(session.value().id);
    ASSERT_TRUE(std::filesystem::exists(staging));

    ASSERT_TRUE(
        drogon::sync_wait(fixture.service.abortUpload(publisher(), session.value().id)).ok());

    EXPECT_FALSE(std::filesystem::exists(staging));
}

// ---------------------------------------------------------------------------
// Quotas
// ---------------------------------------------------------------------------

TEST(UploadServiceTest, RefusesAnUploadThatWouldExceedTheQuota) {
    UploadFixture fixture;
    fixture.users.users.front().uploadQuotaBytes = 10;

    const auto session = drogon::sync_wait(fixture.service.beginUpload(
        publisher(), fixture.buildId, BlobDeclaration{sha256Hex("too big"), 11}));

    ASSERT_FALSE(session.ok());
    EXPECT_EQ(session.error().code, ErrorCode::QuotaExceeded);
}

// Content-addressed storage means the second upload of identical content stores nothing, so
// it must not be charged for either.
TEST(UploadServiceTest, DoesNotChargeTwiceForContentStoredWhileTheUploadWasInFlight) {
    UploadFixture fixture;
    const std::string content = "raced content";
    const auto digest = sha256Hex(content);

    auto session = drogon::sync_wait(
        fixture.service.beginUpload(publisher(),
                                    fixture.buildId,
                                    BlobDeclaration{digest, static_cast<int64_t>(content.size())}));
    ASSERT_TRUE(session.ok());

    // Somebody else finishes storing the very same bytes first.
    fixture.blobs.seed(digest, static_cast<int64_t>(content.size()));

    const auto uploaded =
        drogon::sync_wait(fixture.service.uploadChunk(publisher(), session.value().id, 0, content));

    ASSERT_TRUE(uploaded.ok()) << uploaded.error().detail;
    EXPECT_EQ(fixture.usedBytes(), 0);
}

TEST(UploadServiceTest, RefusesMoreConcurrentUploadsThanTheLimit) {
    UploadSettings settings;
    settings.maxOpenSessionsPerUser = 1;
    UploadFixture fixture(settings);

    ASSERT_TRUE(
        drogon::sync_wait(fixture.service.beginUpload(
                              publisher(), fixture.buildId, BlobDeclaration{sha256Hex("first"), 5}))
            .ok());

    const auto second = drogon::sync_wait(fixture.service.beginUpload(
        publisher(), fixture.buildId, BlobDeclaration{sha256Hex("second"), 6}));

    ASSERT_FALSE(second.ok());
    EXPECT_EQ(second.error().code, ErrorCode::Conflict);
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

TEST(UploadServiceTest, FinalizesABuildAndServesAManifestThatHashesToWhatItRecorded) {
    UploadFixture fixture;
    const std::string executable = "the game binary";
    const std::string data = "the game data";
    ASSERT_TRUE(fixture.upload(executable).ok());
    ASSERT_TRUE(fixture.upload(data).ok());

    const std::vector<ManifestEntry> files{
        ManifestEntry{"Game.exe", sha256Hex(executable), 0, true},
        ManifestEntry{"data/pack.bin", sha256Hex(data), 0, false}};

    const auto finalized = drogon::sync_wait(
        fixture.service.finalizeBuild(publisher(), fixture.buildId, manifestOf(files, "Game.exe")));

    ASSERT_TRUE(finalized.ok()) << finalized.error().detail;
    EXPECT_EQ(finalized.value().status, BuildStatus::Ready);
    EXPECT_EQ(finalized.value().fileCount, 2);
    EXPECT_EQ(finalized.value().totalSizeBytes,
              static_cast<int64_t>(executable.size() + data.size()));

    const auto manifest = drogon::sync_wait(fixture.service.manifest(publisher(), fixture.buildId));
    ASSERT_TRUE(manifest.ok()) << manifest.error().detail;
    EXPECT_EQ(manifest.value().sha256, finalized.value().manifestSha256);
    EXPECT_EQ(sha256Hex(manifest.value().json), manifest.value().sha256)
        << "the served bytes must be exactly the bytes the recorded hash covers";
}

// The declared size is the publisher's claim; the stored size is the fact. A build must not be
// able to advertise a download size its blobs do not have.
TEST(UploadServiceTest, TakesFileSizesFromTheStoreRatherThanFromTheRequest) {
    UploadFixture fixture;
    const std::string content = "twenty-ish bytes";
    ASSERT_TRUE(fixture.upload(content).ok());

    const std::vector<ManifestEntry> files{
        ManifestEntry{"Game.exe", sha256Hex(content), 999999, true}};

    const auto finalized = drogon::sync_wait(
        fixture.service.finalizeBuild(publisher(), fixture.buildId, manifestOf(files, "Game.exe")));

    ASSERT_TRUE(finalized.ok()) << finalized.error().detail;
    EXPECT_EQ(finalized.value().totalSizeBytes, static_cast<int64_t>(content.size()));
}

TEST(UploadServiceTest, RefusesToFinalizeWhileBlobsAreStillMissing) {
    UploadFixture fixture;
    const auto absent = sha256Hex("never uploaded");

    const auto finalized = drogon::sync_wait(fixture.service.finalizeBuild(
        publisher(),
        fixture.buildId,
        manifestOf({ManifestEntry{"Game.exe", absent, 1, true}}, "Game.exe")));

    ASSERT_FALSE(finalized.ok());
    EXPECT_EQ(finalized.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(finalized.error().detail.find(absent), std::string::npos);
}

TEST(UploadServiceTest, RefusesAManifestWithATraversingPath) {
    UploadFixture fixture;
    const std::string content = "payload";
    ASSERT_TRUE(fixture.upload(content).ok());

    const auto finalized = drogon::sync_wait(fixture.service.finalizeBuild(
        publisher(),
        fixture.buildId,
        manifestOf({ManifestEntry{"../escape.exe", sha256Hex(content), 0, true}},
                   "../escape.exe")));

    ASSERT_FALSE(finalized.ok());
    EXPECT_EQ(finalized.error().code, ErrorCode::InvalidInput);
}

TEST(UploadServiceTest, RefusesToFinalizeTheSameBuildTwice) {
    UploadFixture fixture;
    const std::string content = "payload";
    ASSERT_TRUE(fixture.upload(content).ok());
    const auto files =
        manifestOf({ManifestEntry{"Game.exe", sha256Hex(content), 0, true}}, "Game.exe");
    ASSERT_TRUE(
        drogon::sync_wait(fixture.service.finalizeBuild(publisher(), fixture.buildId, files)).ok());

    const auto again =
        drogon::sync_wait(fixture.service.finalizeBuild(publisher(), fixture.buildId, files));

    ASSERT_FALSE(again.ok());
    EXPECT_EQ(again.error().code, ErrorCode::Conflict);
}

TEST(UploadServiceTest, HidesTheManifestOfABuildThatIsNotReady) {
    UploadFixture fixture;

    const auto manifest = drogon::sync_wait(fixture.service.manifest(publisher(), fixture.buildId));

    ASSERT_FALSE(manifest.ok());
    EXPECT_EQ(manifest.error().code, ErrorCode::NotFound);
}

// ---------------------------------------------------------------------------
// Housekeeping
// ---------------------------------------------------------------------------

TEST(UploadServiceTest, SweepsAbandonedSessionsAndTheirStagingFiles) {
    UploadFixture fixture;
    const std::string content = "abandoned halfway";
    auto session = drogon::sync_wait(fixture.service.beginUpload(
        publisher(),
        fixture.buildId,
        BlobDeclaration{sha256Hex(content), static_cast<int64_t>(content.size() + 5)}));
    ASSERT_TRUE(session.ok());
    ASSERT_TRUE(
        drogon::sync_wait(fixture.service.uploadChunk(publisher(), session.value().id, 0, content))
            .ok());

    const launcher::storage::BlobStore store(fixture.root.path());
    const auto staging = store.stagingPathFor(session.value().id);
    ASSERT_TRUE(std::filesystem::exists(staging));

    fixture.sessions.expiredIds.insert(session.value().id);
    const auto reclaimed = drogon::sync_wait(fixture.service.sweepExpiredSessions());

    EXPECT_EQ(reclaimed, 1U);
    EXPECT_FALSE(std::filesystem::exists(staging));
    EXPECT_TRUE(fixture.sessions.sessions.empty());
}

TEST(UploadServiceTest, SweepingLeavesLiveSessionsAlone) {
    UploadFixture fixture;
    ASSERT_TRUE(drogon::sync_wait(
                    fixture.service.beginUpload(publisher(),
                                                fixture.buildId,
                                                BlobDeclaration{sha256Hex("still going"), 11}))
                    .ok());

    EXPECT_EQ(drogon::sync_wait(fixture.service.sweepExpiredSessions()), 0U);
    EXPECT_EQ(fixture.sessions.sessions.size(), 1U);
}

} // namespace
