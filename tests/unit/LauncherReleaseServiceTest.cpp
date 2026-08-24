#include <drogon/drogon.h>
#include <gtest/gtest.h>

#include <string>

#include "domain/LauncherRelease.h"
#include "services/LauncherReleaseService.h"
#include "support/FakeLauncherReleaseRepository.h"
#include "support/ReleaseSigning.h"

namespace {

using launcher::common::ErrorCode;
using launcher::domain::canonicalReleaseDocument;
using launcher::domain::LauncherRelease;
using launcher::domain::ReleaseArch;
using launcher::domain::ReleaseChannel;
using launcher::domain::ReleaseDocument;
using launcher::domain::ReleasePlatform;
using launcher::domain::ReleaseQuery;
using launcher::domain::Semver;
using launcher::services::LauncherReleaseService;
using launcher::services::LauncherReleaseSettings;
using launcher::testing::FakeLauncherReleaseRepository;
using launcher::testing::otherReleasePrivateKey;
using launcher::testing::signWithTestKey;
using launcher::testing::testReleasePublicKey;

constexpr const char* SHA = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";

LauncherRelease releaseFor(const char* version,
                           int major,
                           int minor,
                           int patch,
                           ReleaseChannel channel = ReleaseChannel::Stable,
                           ReleasePlatform platform = ReleasePlatform::Windows,
                           ReleaseArch arch = ReleaseArch::X64) {
    ReleaseDocument document;
    document.channel = channel;
    document.platform = platform;
    document.arch = arch;
    document.version = Semver{major, minor, patch, version};
    document.artifactSha256 = SHA;
    document.artifactSize = 1024;
    document.releasedAt = "2026-08-07T10:00:00Z";

    LauncherRelease release;
    release.id = std::string("release-") + version;
    release.parsed = document;
    release.document = canonicalReleaseDocument(document);
    release.signature = signWithTestKey(release.document);
    release.storageKey =
        std::string(SHA).substr(0, 2) + "/" + std::string(SHA).substr(2, 2) + "/" + SHA + ".zip";
    return release;
}

LauncherReleaseSettings settings() {
    return LauncherReleaseSettings{testReleasePublicKey(), "http://localhost:8081/launcher"};
}

TEST(LauncherReleaseServiceTest, ServesTheDocumentAndItsSignatureUntouched) {
    FakeLauncherReleaseRepository releases;
    const auto stored = releaseFor("0.2.0", 0, 2, 0);
    releases.releases.push_back(stored);
    const LauncherReleaseService service(releases, settings());

    const auto found = drogon::sync_wait(service.latest(ReleaseQuery{}));

    ASSERT_TRUE(found.ok()) << found.error().detail;
    // Byte for byte: the signature covers these, so anything the server did to them on the way
    // out would make the client's check fail for a reason neither side could name.
    EXPECT_EQ(found.value().document, stored.document);
    EXPECT_EQ(found.value().signature, stored.signature);
    EXPECT_EQ(found.value().url, "http://localhost:8081/launcher/" + stored.storageKey);
}

TEST(LauncherReleaseServiceTest, IsOffWithNoSigningKeyConfigured) {
    FakeLauncherReleaseRepository releases;
    releases.releases.push_back(releaseFor("0.2.0", 0, 2, 0));
    const LauncherReleaseService service(
        releases, LauncherReleaseSettings{"", "http://localhost:8081/launcher"});

    EXPECT_FALSE(service.enabled());

    // There is a release in the store and it is not served, because nothing could vouch for it.
    const auto found = drogon::sync_wait(service.latest(ReleaseQuery{}));
    ASSERT_FALSE(found.ok());
    EXPECT_EQ(found.error().code, ErrorCode::NotFound);
}

TEST(LauncherReleaseServiceTest, AnswersNotFoundWhenNothingIsPublished) {
    const FakeLauncherReleaseRepository releases;
    const LauncherReleaseService service(releases, settings());

    const auto found = drogon::sync_wait(service.latest(ReleaseQuery{}));

    ASSERT_FALSE(found.ok());
    EXPECT_EQ(found.error().code, ErrorCode::NotFound);
}

// A row edited in the database is the attacker this design is actually about. The signature
// cannot be forged, so the tamper shows up here — and the release stops being served rather
// than being rejected once per launcher, which is the difference between one log line and a
// fleet quietly failing to update.
TEST(LauncherReleaseServiceTest, RefusesToServeADocumentWhoseSignatureNoLongerCovers) {
    FakeLauncherReleaseRepository releases;
    auto tampered = releaseFor("0.2.0", 0, 2, 0);
    tampered.document.replace(tampered.document.find("0.2.0"), 5, "9.9.9");
    releases.releases.push_back(tampered);
    const LauncherReleaseService service(releases, settings());

    const auto found = drogon::sync_wait(service.latest(ReleaseQuery{}));

    ASSERT_FALSE(found.ok());
    EXPECT_EQ(found.error().code, ErrorCode::NotFound);
}

TEST(LauncherReleaseServiceTest, RefusesToServeADocumentSignedByAnotherKey) {
    FakeLauncherReleaseRepository releases;
    auto foreign = releaseFor("0.2.0", 0, 2, 0);
    foreign.signature = signWithTestKey(foreign.document, otherReleasePrivateKey());
    releases.releases.push_back(foreign);
    const LauncherReleaseService service(releases, settings());

    const auto found = drogon::sync_wait(service.latest(ReleaseQuery{}));

    ASSERT_FALSE(found.ok());
    EXPECT_EQ(found.error().code, ErrorCode::NotFound);
}

// Compared as text 0.10.0 sorts before 0.9.0, which would hand every launcher a downgrade and
// call it the newest release.
TEST(LauncherReleaseServiceTest, PicksTheNumericallyNewestRelease) {
    FakeLauncherReleaseRepository releases;
    releases.releases.push_back(releaseFor("0.9.0", 0, 9, 0));
    releases.releases.push_back(releaseFor("0.10.0", 0, 10, 0));
    releases.releases.push_back(releaseFor("0.8.0", 0, 8, 0));
    const LauncherReleaseService service(releases, settings());

    const auto found = drogon::sync_wait(service.latest(ReleaseQuery{}));

    ASSERT_TRUE(found.ok());
    EXPECT_NE(found.value().document.find(R"("version":"0.10.0")"), std::string::npos);
}

TEST(LauncherReleaseServiceTest, NeverCrossesChannelPlatformOrArchitecture) {
    FakeLauncherReleaseRepository releases;
    releases.releases.push_back(releaseFor("9.9.9", 9, 9, 9, ReleaseChannel::Beta));
    releases.releases.push_back(
        releaseFor("9.9.8", 9, 9, 8, ReleaseChannel::Stable, ReleasePlatform::Linux));
    releases.releases.push_back(releaseFor(
        "9.9.7", 9, 9, 7, ReleaseChannel::Stable, ReleasePlatform::Windows, ReleaseArch::Arm64));
    releases.releases.push_back(releaseFor("0.2.0", 0, 2, 0));
    const LauncherReleaseService service(releases, settings());

    // Three newer releases exist and none of them is this launcher's.
    const auto found = drogon::sync_wait(service.latest(ReleaseQuery{}));

    ASSERT_TRUE(found.ok());
    EXPECT_NE(found.value().document.find(R"("version":"0.2.0")"), std::string::npos);
}

TEST(LauncherReleaseServiceTest, AnswersNotFoundForAPlatformNothingWasPublishedFor) {
    FakeLauncherReleaseRepository releases;
    releases.releases.push_back(releaseFor("0.2.0", 0, 2, 0));
    const LauncherReleaseService service(releases, settings());

    ReleaseQuery macos;
    macos.platform = ReleasePlatform::MacOS;
    const auto found = drogon::sync_wait(service.latest(macos));

    ASSERT_FALSE(found.ok());
    EXPECT_EQ(found.error().code, ErrorCode::NotFound);
}

// Withdrawing a release makes the previous one the newest, and every client then declines it for
// being older than what it is running. Standing still is the intended outcome.
TEST(LauncherReleaseServiceTest, SkipsARetiredRelease) {
    FakeLauncherReleaseRepository releases;
    releases.releases.push_back(releaseFor("0.1.0", 0, 1, 0));
    auto withdrawn = releaseFor("0.2.0", 0, 2, 0);
    withdrawn.retiredAt = "2026-08-07T12:00:00Z";
    releases.releases.push_back(withdrawn);
    const LauncherReleaseService service(releases, settings());

    const auto found = drogon::sync_wait(service.latest(ReleaseQuery{}));

    ASSERT_TRUE(found.ok());
    EXPECT_NE(found.value().document.find(R"("version":"0.1.0")"), std::string::npos);
}

TEST(LauncherReleaseServiceTest, DoesNotDoubleTheSlashOnAConfiguredBaseUrl) {
    FakeLauncherReleaseRepository releases;
    const auto stored = releaseFor("0.2.0", 0, 2, 0);
    releases.releases.push_back(stored);
    const LauncherReleaseService service(
        releases, LauncherReleaseSettings{testReleasePublicKey(), "http://host/launcher/"});

    const auto found = drogon::sync_wait(service.latest(ReleaseQuery{}));

    ASSERT_TRUE(found.ok());
    EXPECT_EQ(found.value().url, "http://host/launcher/" + stored.storageKey);
}

} // namespace
