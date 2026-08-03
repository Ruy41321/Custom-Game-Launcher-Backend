#include <gtest/gtest.h>

#include <drogon/utils/coroutine.h>

#include <string>
#include <vector>

#include "common/Random.h"
#include "domain/Role.h"
#include "services/DownloadService.h"
#include "support/FakeCatalogRepositories.h"
#include "support/FakeDownloadRepository.h"

namespace {

using launcher::common::ErrorCode;
using launcher::domain::Actor;
using launcher::domain::Build;
using launcher::domain::BuildOwnership;
using launcher::domain::BuildStatus;
using launcher::domain::DownloadKind;
using launcher::domain::GameVisibility;
using launcher::domain::ManifestEntry;
using launcher::services::DownloadService;
using launcher::services::DownloadSettings;
using launcher::services::InstalledFile;
using launcher::storage::DownloadUrlSigner;
using launcher::storage::SignedUrlSettings;
using launcher::testing::FakeBuildRepository;
using launcher::testing::FakeDownloadRepository;

namespace permissions = launcher::domain::permissions;

const std::string PUBLISHER = launcher::common::randomUuid();
const std::string PLAYER = launcher::common::randomUuid();

Actor player(std::string userId = PLAYER) {
    return Actor{std::move(userId), {permissions::GAME_READ, permissions::GAME_DOWNLOAD}};
}

Actor publisher() {
    return Actor{PUBLISHER,
                 {permissions::GAME_READ, permissions::GAME_DOWNLOAD, permissions::GAME_PUBLISH}};
}

/// A recognisable stand-in for a content address, and real hex: verification checks the shape
/// of what a client reports, so a filler that is not a hash would be rejected before the
/// comparison under test ever runs.
std::string blob(char marker) {
    static constexpr char HEX[] = "0123456789abcdef";
    const auto value = static_cast<unsigned char>(marker);
    const std::string pair{HEX[value >> 4U], HEX[value & 0x0FU]};

    std::string address;
    while (address.size() < 64) {
        address += pair;
    }
    return address;
}

ManifestEntry file(std::string path, char content, int64_t size = 100) {
    ManifestEntry entry;
    entry.relativePath = std::move(path);
    entry.blobSha256 = blob(content);
    entry.sizeBytes = size;
    return entry;
}

DownloadUrlSigner testSigner() {
    SignedUrlSettings settings;
    settings.publicBaseUrl = "http://files.test/files";
    settings.secret = "signing-secret";
    return DownloadUrlSigner{std::move(settings)};
}

/// Wires the service over fresh fakes and seeds one ready build. Everything is owned by the
/// fixture so the service's references stay valid.
struct DownloadFixture {
    explicit DownloadFixture(DownloadSettings settings = DownloadSettings{})
        : service(builds, downloads, testSigner(), settings) {
        gameId = launcher::common::randomUuid();
        buildId = seedBuild({file("Game.exe", 'a', 10), file("data/pak", 'b', 90)});
    }

    std::string seedBuild(std::vector<ManifestEntry> files,
                          GameVisibility visibility = GameVisibility::Public,
                          BuildStatus status = BuildStatus::Ready,
                          std::string ownerGameId = {}) {
        Build build;
        build.gameVersionId = launcher::common::randomUuid();
        build.status = status;
        build.manifestSha256 = blob('f');
        build.entrypointRelativePath = "Game.exe";
        build.defaultLaunchArgs = "--fullscreen";

        BuildOwnership ownership;
        ownership.gameId = ownerGameId.empty() ? gameId : ownerGameId;
        ownership.publisherUserId = PUBLISHER;
        ownership.visibility = visibility;

        const auto id = builds.seed(build, ownership).id;
        builds.files[id] = std::move(files);
        return id;
    }

    FakeBuildRepository builds;
    FakeDownloadRepository downloads;
    DownloadService service;
    std::string gameId;
    std::string buildId;
};

std::vector<std::string> plannedPaths(const launcher::services::DownloadPlan& plan) {
    std::vector<std::string> paths;
    for (const auto& file : plan.files) {
        paths.push_back(file.entry.relativePath);
    }
    return paths;
}

// ---------------------------------------------------------------------------
// Authorization
// ---------------------------------------------------------------------------

TEST(DownloadServiceTest, RefusesAPlanWithoutTheDownloadPermission) {
    DownloadFixture fixture;
    const Actor stranger{PLAYER, {permissions::GAME_READ}};

    const auto plan = drogon::sync_wait(fixture.service.plan(stranger, fixture.buildId, ""));

    ASSERT_FALSE(plan.ok());
    EXPECT_EQ(plan.error().code, ErrorCode::Forbidden);
}

// A build nobody may see is missing, not forbidden: a 403 would confirm it exists.
TEST(DownloadServiceTest, HidesTheBuildOfADraftGameFromEveryoneButItsPublisher) {
    DownloadFixture fixture;
    const auto draft = fixture.seedBuild({file("Game.exe", 'a')}, GameVisibility::Draft);

    const auto refused = drogon::sync_wait(fixture.service.plan(player(), draft, ""));
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, ErrorCode::NotFound);

    EXPECT_TRUE(drogon::sync_wait(fixture.service.plan(publisher(), draft, "")).ok())
        << "its own publisher can still fetch a draft";
}

TEST(DownloadServiceTest, ABuildStillUploadingIsNotDownloadableYet) {
    DownloadFixture fixture;
    const auto unfinished =
        fixture.seedBuild({file("Game.exe", 'a')}, GameVisibility::Public, BuildStatus::Uploading);

    const auto plan = drogon::sync_wait(fixture.service.plan(player(), unfinished, ""));

    ASSERT_FALSE(plan.ok());
    EXPECT_EQ(plan.error().code, ErrorCode::NotFound);
}

TEST(DownloadServiceTest, RejectsAnUnknownBuildIdWithoutTouchingTheRepository) {
    DownloadFixture fixture;

    const auto plan = drogon::sync_wait(fixture.service.plan(player(), "not-a-uuid", ""));

    ASSERT_FALSE(plan.ok());
    EXPECT_EQ(plan.error().code, ErrorCode::NotFound);
}

TEST(DownloadServiceTest, RefusesToUpdateFromABuildOfAnotherGame) {
    DownloadFixture fixture;
    const auto foreign = fixture.seedBuild({file("Game.exe", 'a')},
                                           GameVisibility::Public,
                                           BuildStatus::Ready,
                                           launcher::common::randomUuid());

    const auto plan = drogon::sync_wait(fixture.service.plan(player(), fixture.buildId, foreign));

    ASSERT_FALSE(plan.ok());
    EXPECT_EQ(plan.error().code, ErrorCode::InvalidInput);
}

// ---------------------------------------------------------------------------
// Plans
// ---------------------------------------------------------------------------

TEST(DownloadServiceTest, PlansAFullDownloadWhenThereIsNothingToUpdateFrom) {
    DownloadFixture fixture;

    const auto plan = drogon::sync_wait(fixture.service.plan(player(), fixture.buildId, ""));

    ASSERT_TRUE(plan.ok());
    EXPECT_EQ(plan.value().kind, DownloadKind::Full);
    EXPECT_EQ(plannedPaths(plan.value()), (std::vector<std::string>{"Game.exe", "data/pak"}));
    EXPECT_EQ(plan.value().downloadBytes, 100);
    EXPECT_EQ(plan.value().entrypointRelativePath, "Game.exe");
    EXPECT_EQ(plan.value().defaultLaunchArgs, "--fullscreen");
}

TEST(DownloadServiceTest, EveryPlannedFileCarriesASignedUrlForItsOwnBlob) {
    DownloadFixture fixture;

    const auto plan = drogon::sync_wait(fixture.service.plan(player(), fixture.buildId, ""));

    ASSERT_TRUE(plan.ok());
    for (const auto& file : plan.value().files) {
        EXPECT_NE(file.url.find("/files/" + file.entry.blobSha256.substr(0, 2) + "/" +
                                file.entry.blobSha256.substr(2, 2) + "/" + file.entry.blobSha256),
                  std::string::npos)
            << file.url;
        EXPECT_NE(file.url.find("token="), std::string::npos);
        EXPECT_NE(file.url.find("expires="), std::string::npos);
    }
}

TEST(DownloadServiceTest, PlansADeltaAgainstTheBuildTheClientAlreadyHas) {
    DownloadFixture fixture;
    const auto installed = fixture.seedBuild(
        {file("Game.exe", 'z', 10), file("data/pak", 'b', 90), file("old", 'q', 1)});

    const auto plan = drogon::sync_wait(fixture.service.plan(player(), fixture.buildId, installed));

    ASSERT_TRUE(plan.ok());
    EXPECT_EQ(plan.value().kind, DownloadKind::Delta);
    EXPECT_EQ(plannedPaths(plan.value()), (std::vector<std::string>{"Game.exe"}));
    EXPECT_EQ(plan.value().remove, (std::vector<std::string>{"old"}));
    EXPECT_EQ(plan.value().unchanged.size(), 1U);
    EXPECT_EQ(plan.value().downloadBytes, 10);
    EXPECT_EQ(plan.value().totalBytes, 100);
}

// Past the threshold a delta costs nearly as much as the build itself, and a full download is
// the simpler thing for a client to get right.
TEST(DownloadServiceTest, FallsBackToAFullDownloadWhenTheDeltaIsNearlyTheWholeBuild) {
    DownloadFixture fixture{DownloadSettings{0.7}};
    const auto installed =
        fixture.seedBuild({file("Game.exe", 'a', 10), file("data/pak", 'y', 90)});

    const auto plan = drogon::sync_wait(fixture.service.plan(player(), fixture.buildId, installed));

    ASSERT_TRUE(plan.ok());
    EXPECT_EQ(plan.value().kind, DownloadKind::Full) << "90 of 100 bytes changed";
    EXPECT_EQ(plan.value().files.size(), 2U);
    EXPECT_TRUE(plan.value().unchanged.empty());
    EXPECT_EQ(plan.value().downloadBytes, 100);
}

TEST(DownloadServiceTest, AClientAlreadyOnTheTargetBuildIsToldToDoNothing) {
    DownloadFixture fixture;

    const auto plan =
        drogon::sync_wait(fixture.service.plan(player(), fixture.buildId, fixture.buildId));

    ASSERT_TRUE(plan.ok());
    EXPECT_TRUE(plan.value().files.empty());
    EXPECT_TRUE(plan.value().remove.empty());
    EXPECT_EQ(plan.value().downloadBytes, 0);
}

// ---------------------------------------------------------------------------
// Analytics
// ---------------------------------------------------------------------------

TEST(DownloadServiceTest, RecordsWhatWasPlannedAndWhichVersionItCameFrom) {
    DownloadFixture fixture;
    const auto installed =
        fixture.seedBuild({file("Game.exe", 'z', 10), file("data/pak", 'b', 90)});
    const auto installedVersion =
        drogon::sync_wait(fixture.builds.findOwnership(installed))->gameVersionId;

    const auto plan = drogon::sync_wait(fixture.service.plan(player(), fixture.buildId, installed));

    ASSERT_TRUE(plan.ok());
    ASSERT_EQ(fixture.downloads.events.size(), 1U);
    const auto& event = fixture.downloads.events.front();
    EXPECT_EQ(event.gameId, fixture.gameId);
    EXPECT_EQ(event.buildId, fixture.buildId);
    EXPECT_EQ(event.userId, PLAYER);
    EXPECT_EQ(event.fromVersionId, installedVersion);
    EXPECT_EQ(event.kind, DownloadKind::Delta);
    EXPECT_EQ(event.bytesPlanned, 10);
}

// Confirming you are up to date is not a download, and counting it would make the statistics
// say the opposite of the truth.
TEST(DownloadServiceTest, DoesNotRecordAPlanWithNothingToFetch) {
    DownloadFixture fixture;

    ASSERT_TRUE(
        drogon::sync_wait(fixture.service.plan(player(), fixture.buildId, fixture.buildId)).ok());

    EXPECT_TRUE(fixture.downloads.events.empty());
}

// ---------------------------------------------------------------------------
// Integrity verification
// ---------------------------------------------------------------------------

TEST(DownloadServiceTest, ReportsAMatchingInstallAsIntact) {
    DownloadFixture fixture;
    const std::vector<InstalledFile> installed{{"Game.exe", blob('a')}, {"data/pak", blob('b')}};

    const auto report =
        drogon::sync_wait(fixture.service.verifyInstall(player(), fixture.buildId, installed));

    ASSERT_TRUE(report.ok());
    EXPECT_TRUE(report.value().intact);
    EXPECT_TRUE(report.value().repair.empty());
    EXPECT_EQ(report.value().repairBytes, 0);
    EXPECT_EQ(report.value().manifestSha256, blob('f'));
}

TEST(DownloadServiceTest, NamesTheMissingAndCorruptFilesAndSignsTheirRepair) {
    DownloadFixture fixture;
    const std::vector<InstalledFile> installed{{"Game.exe", blob('x')}};

    const auto report =
        drogon::sync_wait(fixture.service.verifyInstall(player(), fixture.buildId, installed));

    ASSERT_TRUE(report.ok());
    EXPECT_FALSE(report.value().intact);
    EXPECT_EQ(report.value().corrupt, (std::vector<std::string>{"Game.exe"}));
    EXPECT_EQ(report.value().missing, (std::vector<std::string>{"data/pak"}));
    EXPECT_EQ(report.value().repair.size(), 2U);
    EXPECT_EQ(report.value().repairBytes, 100);
    for (const auto& file : report.value().repair) {
        EXPECT_NE(file.url.find("token="), std::string::npos);
    }
}

// An install directory legitimately accumulates saves, configuration and logs. They are
// reported so the client can decide, not so the server can call the install broken.
TEST(DownloadServiceTest, ReportsFilesTheManifestNeverMentionedWithoutCallingTheInstallBroken) {
    DownloadFixture fixture;
    const std::vector<InstalledFile> installed{
        {"Game.exe", blob('a')}, {"data/pak", blob('b')}, {"saves/slot1.sav", blob('s')}};

    const auto report =
        drogon::sync_wait(fixture.service.verifyInstall(player(), fixture.buildId, installed));

    ASSERT_TRUE(report.ok());
    EXPECT_TRUE(report.value().intact);
    EXPECT_EQ(report.value().unexpected, (std::vector<std::string>{"saves/slot1.sav"}));
    EXPECT_TRUE(report.value().repair.empty());
}

TEST(DownloadServiceTest, RefusesAnInstallReportThatNamesTheSamePathTwice) {
    DownloadFixture fixture;
    const std::vector<InstalledFile> installed{{"Game.exe", blob('a')}, {"Game.exe", blob('x')}};

    const auto report =
        drogon::sync_wait(fixture.service.verifyInstall(player(), fixture.buildId, installed));

    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.error().code, ErrorCode::Conflict);
}

TEST(DownloadServiceTest, RefusesAnInstallReportWhoseHashesAreNotHashes) {
    DownloadFixture fixture;
    const std::vector<InstalledFile> installed{{"Game.exe", "probably-fine"}};

    const auto report =
        drogon::sync_wait(fixture.service.verifyInstall(player(), fixture.buildId, installed));

    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.error().code, ErrorCode::InvalidInput);
}

TEST(DownloadServiceTest, RefusesToVerifyAgainstABuildTheCallerCannotSee) {
    DownloadFixture fixture;
    const auto draft = fixture.seedBuild({file("Game.exe", 'a')}, GameVisibility::Draft);

    const auto report = drogon::sync_wait(
        fixture.service.verifyInstall(player(), draft, {{"Game.exe", blob('a')}}));

    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.error().code, ErrorCode::NotFound);
}

} // namespace
