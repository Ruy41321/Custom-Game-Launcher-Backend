#include <gtest/gtest.h>

#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "common/Hash.h"
#include "integration/AppHarness.h"
#include "storage/BlobStore.h"

namespace {

using launcher::common::sha256Hex;
using launcher::testing::AppHarness;

AppHarness& harness() {
    return *AppHarness::current();
}

std::string uniqueEmail(const std::string& prefix) {
    static std::atomic<int> counter{0};
    return prefix + std::to_string(counter.fetch_add(1)) + "@download.test";
}

Json::Value bodyOf(const drogon::HttpResponsePtr& response) {
    EXPECT_NE(response, nullptr);
    const auto json = response->getJsonObject();
    EXPECT_NE(json, nullptr) << "expected a JSON body";
    return json == nullptr ? Json::Value{} : *json;
}

std::string tokenOf(const Json::Value& session) {
    return session["accessToken"].asString();
}

/// One file of a build, as the publisher sees it before it becomes a blob.
using Content = std::pair<std::string, std::string>;

/// A publisher with a game, able to ship one build after another of it.
struct Publisher {
    Json::Value session;
    std::string gameId;
    int nextVersion{1};

    std::string token() const { return tokenOf(session); }
};

Publisher newPublisher(const std::string& title) {
    Publisher publisher;
    publisher.session = harness().createSessionWithRole(uniqueEmail("publisher"), "dev");

    Json::Value game;
    game["title"] = title;
    game["visibility"] = "public";
    const auto created = harness().postJson("/api/v1/games", game, publisher.token());
    EXPECT_EQ(created->statusCode(), drogon::k201Created) << created->body();
    publisher.gameId = bodyOf(created)["id"].asString();
    return publisher;
}

/// Publishes a complete, ready build: version, build, every blob uploaded, manifest submitted.
/// Returns the build id.
/// The version a build was hung off, remembered by `publish` so a test can act on it.
std::string lastVersionId;

std::string publish(Publisher& publisher,
                    const std::vector<Content>& files,
                    const std::string& visibility = "public",
                    bool publishVersion = true) {
    if (visibility != "public") {
        Json::Value change;
        change["visibility"] = visibility;
        EXPECT_EQ(harness()
                      .patchJson("/api/v1/games/" + publisher.gameId, change, publisher.token())
                      ->statusCode(),
                  drogon::k200OK);
    }

    Json::Value version;
    version["semver"] = "1." + std::to_string(publisher.nextVersion++) + ".0";
    version["publish"] = publishVersion;
    const auto versioned = harness().postJson(
        "/api/v1/games/" + publisher.gameId + "/versions", version, publisher.token());
    EXPECT_EQ(versioned->statusCode(), drogon::k201Created) << versioned->body();
    const auto versionId = bodyOf(versioned)["id"].asString();
    lastVersionId = versionId;

    Json::Value build;
    build["platform"] = "windows";
    const auto built = harness().postJson("/api/v1/games/" + publisher.gameId + "/versions/" +
                                              versionId + "/builds",
                                          build,
                                          publisher.token());
    EXPECT_EQ(built->statusCode(), drogon::k201Created) << built->body();
    const auto buildId = bodyOf(built)["id"].asString();

    Json::Value manifest(Json::arrayValue);
    for (const auto& [path, content] : files) {
        Json::Value declaration;
        declaration["sha256"] = sha256Hex(content);
        declaration["size"] = static_cast<Json::Int64>(content.size());

        const auto started = harness().postJson(
            "/api/v1/builds/" + buildId + "/uploads", declaration, publisher.token());
        // A conflict means the server already holds this content — exactly what content
        // addressing is for, and nothing left to send.
        if (started->statusCode() == drogon::k201Created) {
            const auto sent =
                harness().patchBinary("/api/v1/uploads/" + bodyOf(started)["id"].asString(),
                                      content,
                                      0,
                                      publisher.token());
            EXPECT_EQ(sent->statusCode(), drogon::k200OK) << sent->body();
        } else {
            EXPECT_EQ(started->statusCode(), drogon::k409Conflict) << started->body();
        }

        Json::Value entry;
        entry["path"] = path;
        entry["sha256"] = sha256Hex(content);
        entry["executable"] = false;
        manifest.append(entry);
    }

    Json::Value finalize;
    finalize["files"] = manifest;
    finalize["entrypoint"] = files.front().first;
    finalize["launchArgs"] = "--fullscreen";
    const auto finalized =
        harness().postJson("/api/v1/builds/" + buildId + "/manifest", finalize, publisher.token());
    EXPECT_EQ(finalized->statusCode(), drogon::k200OK) << finalized->body();

    return buildId;
}

drogon::HttpResponsePtr
planFrom(const std::string& buildId, const std::string& fromBuildId, const std::string& token) {
    Json::Value request;
    request["fromBuildId"] = fromBuildId;
    return harness().postJson("/api/v1/builds/" + buildId + "/download", request, token);
}

std::vector<std::string> pathsOf(const Json::Value& files) {
    std::vector<std::string> paths;
    for (const auto& file : files) {
        paths.push_back(file["path"].asString());
    }
    return paths;
}

Json::Value fileNamed(const Json::Value& files, const std::string& path) {
    for (const auto& file : files) {
        if (file["path"].asString() == path) {
            return file;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Plans
// ---------------------------------------------------------------------------

TEST(DownloadEndpointTest, PlansAFullDownloadForAFirstInstall) {
    LAUNCHER_REQUIRE_DATABASE();
    auto publisher = newPublisher("First Install Title");
    const auto buildId =
        publish(publisher, {{"Game.exe", "the binary"}, {"data/pak", "the assets"}});

    // No body at all: a client with nothing installed has nothing to say.
    const auto response =
        harness().post("/api/v1/builds/" + buildId + "/download", publisher.token());

    ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
    const auto plan = bodyOf(response);
    EXPECT_EQ(plan["kind"].asString(), "full");
    EXPECT_EQ(pathsOf(plan["files"]), (std::vector<std::string>{"Game.exe", "data/pak"}));
    EXPECT_EQ(plan["unchanged"].size(), 0U);
    EXPECT_EQ(plan["remove"].size(), 0U);
    EXPECT_EQ(plan["downloadBytes"].asInt64(), 20);
    EXPECT_EQ(plan["totalBytes"].asInt64(), 20);
    EXPECT_EQ(plan["entrypoint"].asString(), "Game.exe");
    EXPECT_FALSE(plan["urlsExpireAt"].asString().empty());
}

// The point of the whole content-addressed layout: a second build that changes one file is one
// file to download, however large the rest of the game is.
TEST(DownloadEndpointTest, PlansADeltaThatOnlyCarriesWhatChanged) {
    LAUNCHER_REQUIRE_DATABASE();
    auto publisher = newPublisher("Delta Update Title");
    const std::string assets = "several megabytes of assets, pretend";
    const auto first = publish(
        publisher, {{"Game.exe", "version one"}, {"data/pak", assets}, {"old.dll", "dropped"}});
    const auto second = publish(publisher, {{"Game.exe", "version two!"}, {"data/pak", assets}});

    const auto response = planFrom(second, first, publisher.token());

    ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
    const auto plan = bodyOf(response);
    EXPECT_EQ(plan["kind"].asString(), "delta");
    EXPECT_EQ(pathsOf(plan["files"]), (std::vector<std::string>{"Game.exe"}));
    EXPECT_EQ(pathsOf(plan["unchanged"]), (std::vector<std::string>{"data/pak"}));
    ASSERT_EQ(plan["remove"].size(), 1U);
    EXPECT_EQ(plan["remove"][0].asString(), "old.dll");
    EXPECT_EQ(plan["downloadBytes"].asInt64(), 12) << "only the changed executable travels";
}

// The URL is the one thing this endpoint exists to produce, so it has to point at a blob that
// is really there, under the layout the file server derives independently.
TEST(DownloadEndpointTest, SignsAUrlThatPointsAtTheStoredBlob) {
    LAUNCHER_REQUIRE_DATABASE();
    auto publisher = newPublisher("Signed Url Title");
    const std::string content = "the bytes a client will fetch";
    const auto buildId = publish(publisher, {{"Game.exe", content}});

    const auto plan =
        bodyOf(harness().post("/api/v1/builds/" + buildId + "/download", publisher.token()));

    ASSERT_EQ(plan["files"].size(), 1U);
    const auto url = plan["files"][0]["url"].asString();
    const auto storageKey = launcher::storage::BlobStore::storageKeyFor(sha256Hex(content));
    EXPECT_NE(url.find("/files/" + storageKey + "?"), std::string::npos) << url;
    EXPECT_NE(url.find("token="), std::string::npos) << url;
    EXPECT_NE(url.find("expires="), std::string::npos) << url;
    EXPECT_TRUE(std::filesystem::exists(harness().blobRoot() / storageKey))
        << "the signed URL must name a blob the file server can actually serve";
}

TEST(DownloadEndpointTest, RecordsWhatWasHandedOut) {
    LAUNCHER_REQUIRE_DATABASE();
    auto publisher = newPublisher("Analytics Title");
    const auto buildId = publish(publisher, {{"Game.exe", "counted bytes"}});

    ASSERT_EQ(
        harness().post("/api/v1/builds/" + buildId + "/download", publisher.token())->statusCode(),
        drogon::k200OK);

    EXPECT_EQ(
        harness().database().scalar(
            "SELECT kind::text || ':' || bytes_planned FROM download_events WHERE build_id = '" +
            buildId + "'::uuid"),
        "full:13");
    EXPECT_EQ(
        harness().database().scalarInt("SELECT count(*) FROM download_events WHERE build_id = '" +
                                       buildId + "'::uuid AND from_version_id IS NULL"),
        1)
        << "a first install comes from no version at all";
}

// ---------------------------------------------------------------------------
// Who may download
// ---------------------------------------------------------------------------

TEST(DownloadEndpointTest, AnyPlayerCanPlanADownloadOfAPublishedGame) {
    LAUNCHER_REQUIRE_DATABASE();
    auto publisher = newPublisher("Public Download Title");
    const auto buildId = publish(publisher, {{"Game.exe", "public bytes"}});
    const auto player = harness().createVerifiedSession(uniqueEmail("player"));

    const auto response =
        harness().post("/api/v1/builds/" + buildId + "/download", tokenOf(player));

    EXPECT_EQ(response->statusCode(), drogon::k200OK) << response->body();
}

TEST(DownloadEndpointTest, HidesTheBuildOfADraftGame) {
    LAUNCHER_REQUIRE_DATABASE();
    auto publisher = newPublisher("Draft Download Title");
    const auto buildId = publish(publisher, {{"Game.exe", "unreleased bytes"}}, "draft");
    const auto player = harness().createVerifiedSession(uniqueEmail("outsider"));

    const auto response =
        harness().post("/api/v1/builds/" + buildId + "/download", tokenOf(player));

    EXPECT_EQ(response->statusCode(), drogon::k404NotFound)
        << "an unreleased build must not be distinguishable from a missing one";
    EXPECT_EQ(
        harness().post("/api/v1/builds/" + buildId + "/download", publisher.token())->statusCode(),
        drogon::k200OK)
        << "its own publisher still tests it";
}

// A public game may carry a version nobody has released yet, and its builds are the
// publisher's alone until it is out. Until 2026-08-17 the check only asked about the game, so
// anybody who could name one of those builds could download it (D62).
TEST(DownloadEndpointTest, HidesEveryBuildOfAVersionThatWasNeverPublished) {
    LAUNCHER_REQUIRE_DATABASE();
    auto publisher = newPublisher("Unreleased Version Title");
    const auto buildId =
        publish(publisher, {{"Game.exe", "not out yet"}}, "public", /*publishVersion=*/false);
    const auto versionId = lastVersionId;
    const auto player = harness().createVerifiedSession(uniqueEmail("outsider"));

    EXPECT_EQ(
        harness().post("/api/v1/builds/" + buildId + "/download", tokenOf(player))->statusCode(),
        drogon::k404NotFound)
        << "404, never 403: a refusal must not confirm there is an unreleased version";
    EXPECT_EQ(
        harness().get("/api/v1/builds/" + buildId + "/manifest", tokenOf(player))->statusCode(),
        drogon::k404NotFound);

    Json::Value report;
    report["files"] = Json::Value(Json::arrayValue);
    EXPECT_EQ(harness()
                  .postJson("/api/v1/builds/" + buildId + "/verify", report, tokenOf(player))
                  ->statusCode(),
              drogon::k404NotFound);

    EXPECT_EQ(
        harness().post("/api/v1/builds/" + buildId + "/download", publisher.token())->statusCode(),
        drogon::k200OK)
        << "its own publisher tests it before releasing the version";

    // And the flag really is what was gating it: publishing the version opens the same build.
    Json::Value release;
    release["published"] = true;
    ASSERT_EQ(harness()
                  .patchJson("/api/v1/games/" + publisher.gameId + "/versions/" + versionId,
                             release,
                             publisher.token())
                  ->statusCode(),
              drogon::k200OK);

    EXPECT_EQ(
        harness().post("/api/v1/builds/" + buildId + "/download", tokenOf(player))->statusCode(),
        drogon::k200OK);
}

// A player who installed a version that was withdrawn afterwards keeps the files on their disk.
// The source of a delta is a claim about that disk, so it costs a full download rather than an
// update the player cannot perform at all.
TEST(DownloadEndpointTest, PlansAFullDownloadWhenTheSourceVersionWasWithdrawn) {
    LAUNCHER_REQUIRE_DATABASE();
    auto publisher = newPublisher("Withdrawn Source Title");
    const std::string assets = "shared assets that would otherwise be skipped";
    const auto installed = publish(publisher, {{"Game.exe", "version one"}, {"data/pak", assets}});
    const auto installedVersion = lastVersionId;
    const auto current = publish(publisher, {{"Game.exe", "version two!"}, {"data/pak", assets}});
    const auto player = harness().createVerifiedSession(uniqueEmail("player"));

    Json::Value withdraw;
    withdraw["published"] = false;
    ASSERT_EQ(harness()
                  .patchJson("/api/v1/games/" + publisher.gameId + "/versions/" + installedVersion,
                             withdraw,
                             publisher.token())
                  ->statusCode(),
              drogon::k200OK);

    const auto response = planFrom(current, installed, tokenOf(player));

    ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
    const auto plan = bodyOf(response);
    EXPECT_EQ(plan["kind"].asString(), "full");
    EXPECT_EQ(plan["unchanged"].size(), 0U)
        << "nothing is skipped on the strength of a build this caller may no longer read";
}

TEST(DownloadEndpointTest, RefusesToPlanAgainstABuildOfAnotherGame) {
    LAUNCHER_REQUIRE_DATABASE();
    auto first = newPublisher("Unrelated First Title");
    auto second = newPublisher("Unrelated Second Title");
    const auto target = publish(first, {{"Game.exe", "one"}});
    const auto foreign = publish(second, {{"Game.exe", "two"}});

    const auto response = planFrom(target, foreign, first.token());

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity) << response->body();
}

TEST(DownloadEndpointTest, RefusesToPlanABuildThatIsNotReady) {
    LAUNCHER_REQUIRE_DATABASE();
    auto publisher = newPublisher("Unfinished Download Title");

    Json::Value version;
    version["semver"] = "9.0.0";
    version["publish"] = true;
    const auto versioned = bodyOf(harness().postJson(
        "/api/v1/games/" + publisher.gameId + "/versions", version, publisher.token()));

    Json::Value build;
    build["platform"] = "windows";
    const auto built = bodyOf(harness().postJson(
        "/api/v1/games/" + publisher.gameId + "/versions/" + versioned["id"].asString() + "/builds",
        build,
        publisher.token()));
    const auto buildId = built["id"].asString();

    const auto response =
        harness().post("/api/v1/builds/" + buildId + "/download", publisher.token());

    EXPECT_EQ(response->statusCode(), drogon::k404NotFound);
}

// ---------------------------------------------------------------------------
// Integrity verification
// ---------------------------------------------------------------------------

Json::Value installReport(const std::vector<Content>& files) {
    Json::Value reported(Json::arrayValue);
    for (const auto& [path, content] : files) {
        Json::Value entry;
        entry["path"] = path;
        entry["sha256"] = sha256Hex(content);
        reported.append(entry);
    }

    Json::Value body;
    body["files"] = reported;
    return body;
}

TEST(DownloadEndpointTest, ConfirmsAnInstallThatMatchesTheManifest) {
    LAUNCHER_REQUIRE_DATABASE();
    auto publisher = newPublisher("Intact Install Title");
    const std::vector<Content> files{{"Game.exe", "binary"}, {"data/pak", "assets"}};
    const auto buildId = publish(publisher, files);

    const auto response = harness().postJson(
        "/api/v1/builds/" + buildId + "/verify", installReport(files), publisher.token());

    ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
    const auto report = bodyOf(response);
    EXPECT_TRUE(report["intact"].asBool());
    EXPECT_EQ(report["repair"].size(), 0U);
    EXPECT_EQ(report["repairBytes"].asInt64(), 0);
}

// The manifest is the authority on what an install must look like, whatever went wrong on the
// way there — a truncated download, a failed disk, a modified file.
TEST(DownloadEndpointTest, NamesTheDamagedFilesAndHandsBackUrlsThatRepairThem) {
    LAUNCHER_REQUIRE_DATABASE();
    auto publisher = newPublisher("Damaged Install Title");
    const auto buildId =
        publish(publisher, {{"Game.exe", "the real binary"}, {"data/pak", "the real assets"}});

    const auto response = harness().postJson(
        "/api/v1/builds/" + buildId + "/verify",
        installReport({{"Game.exe", "a tampered binary"}, {"leftovers.log", "not from the build"}}),
        publisher.token());

    ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
    const auto report = bodyOf(response);
    EXPECT_FALSE(report["intact"].asBool());
    ASSERT_EQ(report["corrupt"].size(), 1U);
    EXPECT_EQ(report["corrupt"][0].asString(), "Game.exe");
    ASSERT_EQ(report["missing"].size(), 1U);
    EXPECT_EQ(report["missing"][0].asString(), "data/pak");
    ASSERT_EQ(report["unexpected"].size(), 1U);
    EXPECT_EQ(report["unexpected"][0].asString(), "leftovers.log");

    EXPECT_EQ(report["repair"].size(), 2U);
    EXPECT_FALSE(fileNamed(report["repair"], "Game.exe")["url"].asString().empty());
    EXPECT_EQ(report["repairBytes"].asInt64(), 30);
}

TEST(DownloadEndpointTest, RefusesAnInstallReportThatIsNotHashes) {
    LAUNCHER_REQUIRE_DATABASE();
    auto publisher = newPublisher("Bad Report Title");
    const auto buildId = publish(publisher, {{"Game.exe", "binary"}});

    Json::Value file;
    file["path"] = "Game.exe";
    file["sha256"] = "trust me";
    Json::Value files(Json::arrayValue);
    files.append(file);
    Json::Value body;
    body["files"] = files;

    const auto response =
        harness().postJson("/api/v1/builds/" + buildId + "/verify", body, publisher.token());

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity) << response->body();
}

} // namespace
