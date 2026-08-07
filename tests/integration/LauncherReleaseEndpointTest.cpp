#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/Config.h"
#include "app/ReleasePublish.h"
#include "common/Hash.h"
#include "domain/LauncherRelease.h"
#include "integration/AppHarness.h"
#include "support/ReleaseSigning.h"
#include "support/TemporaryDirectory.h"

namespace {

using launcher::app::AppConfig;
using launcher::app::publishRelease;
using launcher::app::ReleasePublishOutcome;
using launcher::app::ReleasePublishRequest;
using launcher::app::ReleaseRetireOutcome;
using launcher::app::retireRelease;
using launcher::common::sha256Hex;
using launcher::domain::ReleaseArch;
using launcher::domain::ReleaseChannel;
using launcher::domain::ReleasePlatform;
using launcher::domain::ReleaseQuery;
using launcher::testing::AppHarness;
using launcher::testing::otherReleasePrivateKey;
using launcher::testing::signWithTestKey;
using launcher::testing::TemporaryDirectory;
using launcher::testing::testReleasePublicKey;

AppHarness& harness() {
    return *AppHarness::current();
}

/// A configuration pointing at the harness database and its release root, as the command has
/// when it runs with no server: it opens its own libpq connection rather than borrowing
/// Drogon's.
AppConfig publishConfig() {
    AppConfig config;
    config.database.name = harness().database().name();
    const auto* host = std::getenv("LAUNCHER_TEST_DB_HOST");
    const auto* user = std::getenv("LAUNCHER_TEST_DB_USER");
    const auto* password = std::getenv("LAUNCHER_TEST_DB_PASSWORD");
    config.database.host = host == nullptr ? "" : host;
    config.database.user = user == nullptr ? "postgres" : user;
    config.database.password = password == nullptr ? "" : password;

    config.launcherReleases.root = harness().releaseRoot().string();
    config.launcherReleases.publicKey = testReleasePublicKey();
    return config;
}

/// What the maintainer's own machine produces: an artifact, a document naming its hash, and a
/// detached signature over the document's exact bytes.
struct SignedRelease {
    std::string document;
    std::string signature;
    std::string artifact;
    std::string sha256;
};

SignedRelease makeRelease(const std::string& version,
                          const std::string& channel = "stable",
                          const std::string& platform = "windows",
                          const std::string& arch = "x64") {
    SignedRelease release;
    release.artifact = "PK\x03\x04 launcher " + version + " for " + platform + "/" + arch;
    release.sha256 = sha256Hex(release.artifact);
    release.document = R"({"schema":1,"channel":")" + channel + R"(","version":")" + version +
                       R"(","platform":")" + platform + R"(","arch":")" + arch + R"(","sha256":")" +
                       release.sha256 + R"(","size":)" + std::to_string(release.artifact.size()) +
                       R"(,"releasedAt":"2026-08-07T10:00:00Z","notes":"Notes for )" + version +
                       R"("})";
    release.signature = signWithTestKey(release.document);
    return release;
}

/// Writes the three files a publish takes and returns the request naming them.
ReleasePublishRequest onDisk(const TemporaryDirectory& directory, const SignedRelease& release) {
    const auto write = [](const std::filesystem::path& path, const std::string& body) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(body.data(), static_cast<std::streamsize>(body.size()));
        return path;
    };

    ReleasePublishRequest request;
    request.documentPath = write(directory.path() / "release.json", release.document);
    request.signaturePath = write(directory.path() / "release.json.sig", release.signature);
    request.artifactPath = write(directory.path() / "launcher.zip", release.artifact);
    return request;
}

std::string latestPath(const std::string& channel = "stable",
                       const std::string& platform = "windows",
                       const std::string& arch = "x64") {
    return "/api/v1/launcher/releases/latest?channel=" + channel + "&platform=" + platform +
           "&arch=" + arch;
}

Json::Value bodyOf(const drogon::HttpResponsePtr& response) {
    Json::Value body;
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    const auto view = response->body();
    std::string errors;
    reader->parse(view.data(), view.data() + view.size(), &body, &errors);
    return body;
}

// A launcher too old to sign in, pointed at a server it has never reached, still has to be able
// to ask. That is why this route takes no token at all.
TEST(LauncherReleaseEndpointTest, IsReadableWithNoToken) {
    LAUNCHER_REQUIRE_DATABASE();
    TemporaryDirectory files;
    const auto release = makeRelease("3.1.0", "stable", "linux", "arm64");
    ASSERT_TRUE(publishRelease(publishConfig(), onDisk(files, release)).ok());

    const auto response = harness().get(latestPath("stable", "linux", "arm64"));

    ASSERT_EQ(response->statusCode(), drogon::k200OK);
    const auto body = bodyOf(response);
    // Byte for byte what was signed: the client verifies the signature over exactly this.
    EXPECT_EQ(body["document"].asString(), release.document);
    EXPECT_EQ(body["signature"].asString(), release.signature);
    EXPECT_EQ(body["url"].asString(),
              "http://files.test/launcher/" + release.sha256.substr(0, 2) + "/" +
                  release.sha256.substr(2, 2) + "/" + release.sha256 + ".zip");
}

TEST(LauncherReleaseEndpointTest, AnswersNotFoundForAPlatformWithNoRelease) {
    LAUNCHER_REQUIRE_DATABASE();

    const auto response = harness().get(latestPath("beta", "macos", "arm64"));

    // Not "no such platform" and not "this deployment does not build for macOS": from a client's
    // side there is one situation, which is that there is nothing to update to.
    EXPECT_EQ(response->statusCode(), drogon::k404NotFound);
}

TEST(LauncherReleaseEndpointTest, RequiresAPlatformAndAnArchitecture) {
    LAUNCHER_REQUIRE_DATABASE();

    EXPECT_EQ(harness().get("/api/v1/launcher/releases/latest")->statusCode(),
              drogon::k422UnprocessableEntity);
    EXPECT_EQ(harness().get("/api/v1/launcher/releases/latest?platform=windows")->statusCode(),
              drogon::k422UnprocessableEntity);
    EXPECT_EQ(harness().get("/api/v1/launcher/releases/latest?arch=x64")->statusCode(),
              drogon::k422UnprocessableEntity);
    // The runtime identifier rather than the platform name — a real mistake, and one that must
    // not silently resolve to something.
    EXPECT_EQ(harness().get(latestPath("stable", "osx", "arm64"))->statusCode(),
              drogon::k422UnprocessableEntity);
}

// A typo is refused rather than read as `stable`: quietly moving a caller onto a stream they did
// not name is exactly what a channel exists to prevent.
TEST(LauncherReleaseEndpointTest, RefusesAChannelItDoesNotKnow) {
    LAUNCHER_REQUIRE_DATABASE();

    const auto response = harness().get(latestPath("nightly", "windows", "x64"));

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
}

// A client too old to know about channels gets the stream everybody is on, never a pre-release.
TEST(LauncherReleaseEndpointTest, DefaultsToTheStableChannel) {
    LAUNCHER_REQUIRE_DATABASE();
    TemporaryDirectory files;
    const auto stable = makeRelease("4.0.0", "stable", "macos", "x64");
    const auto beta = makeRelease("5.0.0", "beta", "macos", "x64");
    ASSERT_TRUE(publishRelease(publishConfig(), onDisk(files, stable)).ok());
    TemporaryDirectory betaFiles;
    ASSERT_TRUE(publishRelease(publishConfig(), onDisk(betaFiles, beta)).ok());

    const auto response = harness().get("/api/v1/launcher/releases/latest?platform=macos&arch=x64");

    ASSERT_EQ(response->statusCode(), drogon::k200OK);
    EXPECT_EQ(bodyOf(response)["document"].asString(), stable.document);
}

TEST(LauncherReleaseEndpointTest, OrdersReleasesNumericallyRatherThanAsText) {
    LAUNCHER_REQUIRE_DATABASE();
    TemporaryDirectory nine;
    TemporaryDirectory ten;
    const auto older = makeRelease("6.9.0", "stable", "linux", "x64");
    const auto newer = makeRelease("6.10.0", "stable", "linux", "x64");
    ASSERT_TRUE(publishRelease(publishConfig(), onDisk(nine, older)).ok());
    ASSERT_TRUE(publishRelease(publishConfig(), onDisk(ten, newer)).ok());

    const auto response = harness().get(latestPath("stable", "linux", "x64"));

    ASSERT_EQ(response->statusCode(), drogon::k200OK);
    // As text, 6.10.0 sorts before 6.9.0 — which would offer a downgrade as the newest release.
    EXPECT_EQ(bodyOf(response)["document"].asString(), newer.document);
}

TEST(LauncherReleaseEndpointTest, StoresTheArtifactAtItsContentAddress) {
    LAUNCHER_REQUIRE_DATABASE();
    TemporaryDirectory files;
    const auto release = makeRelease("7.0.0", "beta", "windows", "arm64");

    ASSERT_TRUE(publishRelease(publishConfig(), onDisk(files, release)).ok());

    const auto stored = harness().releaseRoot() / release.sha256.substr(0, 2) /
                        release.sha256.substr(2, 2) / (release.sha256 + ".zip");
    ASSERT_TRUE(std::filesystem::is_regular_file(stored));
    std::ifstream file(stored, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    EXPECT_EQ(bytes, release.artifact);
}

// The whole reason the private key lives off this machine. Somebody who can write to this
// server's disk and database still has to produce a signature to publish anything.
TEST(LauncherReleaseEndpointTest, RefusesToPublishADocumentSignedByAnotherKey) {
    LAUNCHER_REQUIRE_DATABASE();
    TemporaryDirectory files;
    auto release = makeRelease("8.0.0", "stable", "linux", "arm64");
    release.signature = signWithTestKey(release.document, otherReleasePrivateKey());

    const auto published = publishRelease(publishConfig(), onDisk(files, release));

    ASSERT_FALSE(published.ok());
    EXPECT_EQ(harness().get(latestPath("stable", "linux", "arm64"))->statusCode(),
              drogon::k404NotFound);
}

TEST(LauncherReleaseEndpointTest, RefusesToPublishADocumentChangedAfterItWasSigned) {
    LAUNCHER_REQUIRE_DATABASE();
    TemporaryDirectory files;
    auto release = makeRelease("8.1.0", "beta", "linux", "arm64");
    // Signed as 8.1.0, offered as 9.9.9 — the version is inside what the signature covers.
    release.document.replace(release.document.find("8.1.0"), 5, "9.9.9");

    const auto published = publishRelease(publishConfig(), onDisk(files, release));

    ASSERT_FALSE(published.ok());
    EXPECT_EQ(harness().get(latestPath("beta", "linux", "arm64"))->statusCode(),
              drogon::k404NotFound);
}

// The step that ties a signature to a file. Everything else proves somebody signed a
// description; this proves the file is the thing described.
TEST(LauncherReleaseEndpointTest, RefusesToPublishAnArtifactThatDoesNotMatchTheSignedHash) {
    LAUNCHER_REQUIRE_DATABASE();
    TemporaryDirectory files;
    auto release = makeRelease("8.2.0", "beta", "windows", "x64");
    const auto request = onDisk(files, release);
    // The document and its signature are untouched; the file underneath is not the one it names.
    {
        std::ofstream file(request.artifactPath, std::ios::binary | std::ios::trunc);
        file << "a different launcher entirely";
    }

    const auto published = publishRelease(publishConfig(), request);

    ASSERT_FALSE(published.ok());
    EXPECT_NE(published.error().detail.find("does not match the hash"), std::string::npos);
}

TEST(LauncherReleaseEndpointTest, RefusesADocumentThatIsNotInCanonicalForm) {
    LAUNCHER_REQUIRE_DATABASE();
    TemporaryDirectory files;
    auto release = makeRelease("8.3.0", "beta", "macos", "arm64");
    // What a text editor adds on save. It is signed correctly — and it is not the document.
    release.document += "\n";
    release.signature = signWithTestKey(release.document);

    const auto published = publishRelease(publishConfig(), onDisk(files, release));

    ASSERT_FALSE(published.ok());
    EXPECT_NE(published.error().detail.find("canonical"), std::string::npos);
}

TEST(LauncherReleaseEndpointTest, RefusesToPublishTheSameVersionTwice) {
    LAUNCHER_REQUIRE_DATABASE();
    TemporaryDirectory files;
    const auto release = makeRelease("9.0.0", "stable", "windows", "arm64");
    const auto request = onDisk(files, release);

    const auto first = publishRelease(publishConfig(), request);
    const auto second = publishRelease(publishConfig(), request);

    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value(), ReleasePublishOutcome::Published);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value(), ReleasePublishOutcome::AlreadyPublished);
}

TEST(LauncherReleaseEndpointTest, RetiringAReleaseOffersTheOneBeforeIt) {
    LAUNCHER_REQUIRE_DATABASE();
    TemporaryDirectory oldFiles;
    TemporaryDirectory badFiles;
    const auto good = makeRelease("10.1.0", "beta", "windows", "x64");
    const auto bad = makeRelease("10.2.0", "beta", "windows", "x64");
    ASSERT_TRUE(publishRelease(publishConfig(), onDisk(oldFiles, good)).ok());
    ASSERT_TRUE(publishRelease(publishConfig(), onDisk(badFiles, bad)).ok());
    ASSERT_EQ(bodyOf(harness().get(latestPath("beta", "windows", "x64")))["document"].asString(),
              bad.document);

    ReleaseQuery query;
    query.channel = ReleaseChannel::Beta;
    query.platform = ReleasePlatform::Windows;
    query.arch = ReleaseArch::X64;
    const auto retired = retireRelease(publishConfig(), query, "10.2.0");

    ASSERT_TRUE(retired.ok());
    EXPECT_EQ(retired.value(), ReleaseRetireOutcome::Retired);
    // The previous release becomes the newest, and every launcher already on 10.2.0 declines it
    // for being older than what it is running. Standing still is the intended outcome.
    EXPECT_EQ(bodyOf(harness().get(latestPath("beta", "windows", "x64")))["document"].asString(),
              good.document);
}

TEST(LauncherReleaseEndpointTest, RetiringSomethingThatIsNotLiveSaysSo) {
    LAUNCHER_REQUIRE_DATABASE();
    ReleaseQuery query;
    query.platform = ReleasePlatform::MacOS;
    query.arch = ReleaseArch::Arm64;

    const auto retired = retireRelease(publishConfig(), query, "99.0.0");

    ASSERT_TRUE(retired.ok());
    EXPECT_EQ(retired.value(), ReleaseRetireOutcome::NotLive);
}

// A deployment that has not set up signing publishes nothing rather than publishing something
// nobody can check.
TEST(LauncherReleaseEndpointTest, RefusesToPublishWithNoSigningKeyConfigured) {
    LAUNCHER_REQUIRE_DATABASE();
    TemporaryDirectory files;
    auto config = publishConfig();
    config.launcherReleases.publicKey.clear();

    const auto published =
        publishRelease(config, onDisk(files, makeRelease("11.0.0", "stable", "macos", "x64")));

    ASSERT_FALSE(published.ok());
    EXPECT_NE(published.error().detail.find("LAUNCHER_RELEASE_PUBLIC_KEY"), std::string::npos);
}

TEST(LauncherReleaseEndpointTest, CapabilitiesSayWhetherTheSurfaceIsOn) {
    const auto response = harness().get("/api/v1/capabilities");

    ASSERT_EQ(response->statusCode(), drogon::k200OK);
    const auto body = bodyOf(response);
    EXPECT_TRUE(body["launcherReleases"]["enabled"].asBool());
    ASSERT_EQ(body["launcherReleases"]["channels"].size(), 2U);
    EXPECT_EQ(body["launcherReleases"]["channels"][0].asString(), "stable");
    EXPECT_EQ(body["launcherReleases"]["channels"][1].asString(), "beta");
}

} // namespace
