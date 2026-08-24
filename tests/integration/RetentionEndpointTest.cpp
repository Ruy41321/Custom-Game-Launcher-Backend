#include <gtest/gtest.h>

#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <json/json.h>

#include <atomic>
#include <filesystem>
#include <string>

#include "app/AppContext.h"
#include "common/Hash.h"
#include "integration/AppHarness.h"
#include "services/RetentionService.h"
#include "storage/BlobStore.h"

namespace {

using launcher::common::sha256Hex;
using launcher::storage::BlobStore;
using launcher::testing::AppHarness;

AppHarness& harness() {
    return *AppHarness::current();
}

std::string uniqueEmail(const std::string& prefix) {
    static std::atomic<int> counter{0};
    return prefix + std::to_string(counter.fetch_add(1)) + "@example.test";
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

struct Publication {
    Json::Value session;
    std::string gameId;
    std::string versionId;
    std::string buildId;

    std::string token() const { return tokenOf(session); }
};

/// A game with a published version and one `ready` build carrying a single file.
Publication publishedBuild(const std::string& title, const std::string& content) {
    Publication publication;
    publication.session = harness().createSessionWithRole(uniqueEmail("retention"), "dev");

    Json::Value game;
    game["title"] = title;
    game["visibility"] = "public";
    publication.gameId =
        bodyOf(harness().postJson("/api/v1/games", game, publication.token()))["id"].asString();

    Json::Value version;
    version["semver"] = "1.0.0";
    version["publish"] = true;
    publication.versionId = bodyOf(harness().postJson(
        "/api/v1/games/" + publication.gameId + "/versions", version, publication.token()))["id"]
                                .asString();

    Json::Value build;
    build["platform"] = "windows";
    publication.buildId = bodyOf(harness().postJson(
        "/api/v1/games/" + publication.gameId + "/versions/" + publication.versionId + "/builds",
        build,
        publication.token()))["id"]
                              .asString();

    Json::Value declaration;
    declaration["sha256"] = sha256Hex(content);
    declaration["size"] = static_cast<Json::Int64>(content.size());
    const auto negotiated = harness().postJson(
        "/api/v1/builds/" + publication.buildId + "/uploads", declaration, publication.token());

    // A 409 here is the deduplication working: the server already holds these bytes from an
    // earlier build, so there is no session and nothing to send. The manifest below still
    // names the blob, which is exactly how two builds come to share one file.
    if (negotiated->statusCode() != drogon::k409Conflict) {
        EXPECT_EQ(negotiated->statusCode(), drogon::k201Created) << negotiated->body();
        const auto session = bodyOf(negotiated);
        EXPECT_EQ(
            harness()
                .patchBinary(
                    "/api/v1/uploads/" + session["id"].asString(), content, 0, publication.token())
                ->statusCode(),
            drogon::k200OK);
    }

    Json::Value entry;
    entry["path"] = "Game.exe";
    entry["sha256"] = sha256Hex(content);
    entry["executable"] = true;
    Json::Value manifest;
    manifest["files"] = Json::Value(Json::arrayValue);
    manifest["files"].append(entry);
    manifest["entrypoint"] = "Game.exe";
    EXPECT_EQ(harness()
                  .postJson("/api/v1/builds/" + publication.buildId + "/manifest",
                            manifest,
                            publication.token())
                  ->statusCode(),
              drogon::k200OK);

    return publication;
}

// ---------------------------------------------------------------------------
// Deleting builds and versions
// ---------------------------------------------------------------------------

TEST(RetentionEndpointTest, DeletesABuildItsPublisherOwns) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = publishedBuild("Deletable Build", "windows binary");

    const auto response =
        harness().remove("/api/v1/builds/" + publication.buildId, publication.token());

    ASSERT_EQ(response->statusCode(), drogon::k204NoContent) << response->body();
    // Gone means gone: the manifest that described it is no longer servable either.
    EXPECT_EQ(harness()
                  .get("/api/v1/builds/" + publication.buildId + "/manifest", publication.token())
                  ->statusCode(),
              drogon::k404NotFound);
}

TEST(RetentionEndpointTest, RefusesToDeleteABuildBelongingToAnotherPublisher) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = publishedBuild("Somebody Elses Build", "not yours");
    const auto stranger = harness().createSessionWithRole(uniqueEmail("stranger"), "dev");

    const auto response =
        harness().remove("/api/v1/builds/" + publication.buildId, tokenOf(stranger));

    EXPECT_EQ(response->statusCode(), drogon::k403Forbidden) << response->body();
}

TEST(RetentionEndpointTest, APlayerCannotDeleteABuild) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = publishedBuild("Player Proof Build", "still not yours");
    const auto player = harness().createVerifiedSession(uniqueEmail("retentionplayer"));

    const auto response =
        harness().remove("/api/v1/builds/" + publication.buildId, tokenOf(player));

    EXPECT_EQ(response->statusCode(), drogon::k403Forbidden) << response->body();
}

TEST(RetentionEndpointTest, ReportsAnUnknownBuildAsMissing) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = harness().createSessionWithRole(uniqueEmail("retention"), "dev");

    EXPECT_EQ(harness()
                  .remove("/api/v1/builds/00000000-0000-4000-8000-000000000000", tokenOf(session))
                  ->statusCode(),
              drogon::k404NotFound);
    EXPECT_EQ(harness().remove("/api/v1/builds/not-a-uuid", tokenOf(session))->statusCode(),
              drogon::k404NotFound);
}

TEST(RetentionEndpointTest, DeletingAVersionTakesItsBuildsWithIt) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = publishedBuild("Deletable Version", "version content");

    const auto response = harness().remove("/api/v1/games/" + publication.gameId + "/versions/" +
                                               publication.versionId,
                                           publication.token());

    ASSERT_EQ(response->statusCode(), drogon::k204NoContent) << response->body();
    const auto detail =
        bodyOf(harness().get("/api/v1/games/" + publication.gameId, publication.token()));
    EXPECT_EQ(detail["versions"].size(), 0u);
    EXPECT_EQ(detail["builds"].size(), 0u) << "the builds cascade with the version";
}

TEST(RetentionEndpointTest, RefusesToDeleteAVersionOfSomebodyElsesGame) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = publishedBuild("Guarded Version", "guarded");
    const auto stranger = harness().createSessionWithRole(uniqueEmail("stranger"), "dev");

    const auto response = harness().remove("/api/v1/games/" + publication.gameId + "/versions/" +
                                               publication.versionId,
                                           tokenOf(stranger));

    EXPECT_EQ(response->statusCode(), drogon::k403Forbidden) << response->body();
}

TEST(RetentionEndpointTest, AVersionOfAnotherGameIsMissingRatherThanRefused) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto mine = publishedBuild("My Own Game", "mine");
    const auto other = publishedBuild("Another Game", "theirs");

    // The caller owns the game in the path and named a version that is not under it. That is a
    // path which does not exist, not one they may not use.
    const auto response = harness().remove(
        "/api/v1/games/" + mine.gameId + "/versions/" + other.versionId, mine.token());

    EXPECT_EQ(response->statusCode(), drogon::k404NotFound) << response->body();
}

// ---------------------------------------------------------------------------
// Deleting a game
// ---------------------------------------------------------------------------

/// Uploads a cover and returns the path the file server would serve it from.
std::filesystem::path uploadCover(const Publication& publication, const std::string& suffix) {
    const std::string body = std::string("\x89PNG\r\n\x1a\n", 8) + suffix;
    const auto response = harness().postBinary("/api/v1/games/" + publication.gameId + "/media",
                                               {{"kind", "cover"}},
                                               body,
                                               publication.token());
    EXPECT_EQ(response->statusCode(), drogon::k201Created) << response->body();

    const auto url = bodyOf(response)["url"].asString();
    const std::string base = "http://files.test/media/";
    EXPECT_EQ(url.rfind(base, 0), 0u) << "unexpected media URL: " << url;
    return harness().mediaRoot() / url.substr(base.size());
}

TEST(RetentionEndpointTest, DeletingAGameTakesEverythingUnderItAndItsArtwork) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = publishedBuild("Deletable Game", "a whole game");
    const auto cover = uploadCover(publication, "deletable game");
    ASSERT_TRUE(std::filesystem::exists(cover));

    const auto response =
        harness().remove("/api/v1/games/" + publication.gameId, publication.token());

    ASSERT_EQ(response->statusCode(), drogon::k204NoContent) << response->body();
    EXPECT_EQ(
        harness().get("/api/v1/games/" + publication.gameId, publication.token())->statusCode(),
        drogon::k404NotFound);
    // The build went with it, so the manifest an installed copy would update from is gone too.
    EXPECT_EQ(harness()
                  .get("/api/v1/builds/" + publication.buildId + "/manifest", publication.token())
                  ->statusCode(),
              drogon::k404NotFound);
    EXPECT_FALSE(std::filesystem::exists(cover))
        << "no row points at the picture any more, so the file should not survive either";
}

TEST(RetentionEndpointTest, ASecondDeleteOfAGameIsMissingRatherThanForbidden) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = publishedBuild("Twice Deleted", "gone once");
    ASSERT_EQ(
        harness().remove("/api/v1/games/" + publication.gameId, publication.token())->statusCode(),
        drogon::k204NoContent);

    const auto again = harness().remove("/api/v1/games/" + publication.gameId, publication.token());

    EXPECT_EQ(again->statusCode(), drogon::k404NotFound) << again->body();
}

TEST(RetentionEndpointTest, RefusesToDeleteAGameBelongingToAnotherPublisher) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = publishedBuild("Not Yours To Delete", "still theirs");
    const auto stranger = harness().createSessionWithRole(uniqueEmail("stranger"), "dev");

    const auto response =
        harness().remove("/api/v1/games/" + publication.gameId, tokenOf(stranger));

    EXPECT_EQ(response->statusCode(), drogon::k403Forbidden) << response->body();
    EXPECT_EQ(
        harness().get("/api/v1/games/" + publication.gameId, publication.token())->statusCode(),
        drogon::k200OK);
}

TEST(RetentionEndpointTest, DeletesAGameThatSomebodyElseHasInTheirLibrary) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = publishedBuild("Somebody Elses Shelf", "shelved");
    const auto player = harness().createVerifiedSession(uniqueEmail("shelf"));
    ASSERT_EQ(harness().put("/api/v1/library/" + publication.gameId, tokenOf(player))->statusCode(),
              drogon::k200OK);

    // A library entry is a bookmark, not a licence: refusing here would let one stranger stop a
    // publisher from ever withdrawing their own work.
    ASSERT_EQ(
        harness().remove("/api/v1/games/" + publication.gameId, publication.token())->statusCode(),
        drogon::k204NoContent);

    const auto library = bodyOf(harness().get("/api/v1/library", tokenOf(player)));
    for (const auto& game : library["items"]) {
        EXPECT_NE(game["id"].asString(), publication.gameId);
    }
}

TEST(RetentionEndpointTest, TheSweepReclaimsTheBlobsOfADeletedGame) {
    LAUNCHER_REQUIRE_DATABASE();
    const std::string content = "bytes that outlive their game";
    const auto publication = publishedBuild("Collected Game", content);
    const auto digest = sha256Hex(content);
    const BlobStore store(harness().blobRoot());
    ASSERT_TRUE(store.contains(digest));

    ASSERT_EQ(
        harness().remove("/api/v1/games/" + publication.gameId, publication.token())->statusCode(),
        drogon::k204NoContent);

    // Deleting the rows never touches a blob: another build may hold the same bytes, and that
    // question belongs to the collector, one grace period later. The harness runs with none.
    EXPECT_TRUE(store.contains(digest));
    const auto swept = drogon::sync_wait(
        launcher::app::AppContext::instance().retentionService().collectUnreferencedBlobs());

    EXPECT_GE(swept.collected, 1u);
    EXPECT_FALSE(store.contains(digest));
}

// ---------------------------------------------------------------------------
// Collecting what deletion left behind
// ---------------------------------------------------------------------------

TEST(RetentionEndpointTest, TheSweepReclaimsTheDiskAndTheQuotaOfADeletedBuild) {
    LAUNCHER_REQUIRE_DATABASE();
    const std::string content = "bytes that outlive their build";
    const auto publication = publishedBuild("Collected Build", content);
    const auto digest = sha256Hex(content);
    const BlobStore store(harness().blobRoot());
    ASSERT_TRUE(store.contains(digest)) << "the publish should have stored the blob";

    // The harness runs with no grace period, so one pass is enough to observe what a deployed
    // server would do a day later.
    const auto& retention = launcher::app::AppContext::instance().retentionService();
    const auto before = drogon::sync_wait(retention.collectUnreferencedBlobs());
    EXPECT_EQ(before.collected, 0u)
        << "a blob a live manifest references must never be collectable";
    EXPECT_TRUE(store.contains(digest));

    ASSERT_EQ(harness()
                  .remove("/api/v1/builds/" + publication.buildId, publication.token())
                  ->statusCode(),
              drogon::k204NoContent);

    const auto after = drogon::sync_wait(retention.collectUnreferencedBlobs());

    EXPECT_GE(after.collected, 1u);
    EXPECT_FALSE(store.contains(digest)) << "nothing references it any more, so the disk goes back";
}

TEST(RetentionEndpointTest, TheSweepLeavesABlobTwoBuildsShare) {
    LAUNCHER_REQUIRE_DATABASE();
    const std::string content = "content two builds agree on";
    const auto first = publishedBuild("Sharing One", content);
    const auto second = publishedBuild("Sharing Two", content);
    const auto digest = sha256Hex(content);
    const BlobStore store(harness().blobRoot());

    ASSERT_EQ(harness().remove("/api/v1/builds/" + first.buildId, first.token())->statusCode(),
              drogon::k204NoContent);
    drogon::sync_wait(
        launcher::app::AppContext::instance().retentionService().collectUnreferencedBlobs());

    // Content-addressed storage means one file serves both builds, so deleting one build must
    // not take the bytes the other one still needs.
    EXPECT_TRUE(store.contains(digest));
    EXPECT_EQ(harness()
                  .get("/api/v1/builds/" + second.buildId + "/manifest", second.token())
                  ->statusCode(),
              drogon::k200OK);
}

} // namespace
