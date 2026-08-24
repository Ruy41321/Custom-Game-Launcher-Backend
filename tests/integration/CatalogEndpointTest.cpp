#include <gtest/gtest.h>

#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <atomic>
#include <string>

#include "integration/AppHarness.h"

namespace {

using launcher::testing::AppHarness;

AppHarness& harness() {
    return *AppHarness::current();
}

std::string uniqueEmail(const std::string& prefix) {
    static std::atomic<int> counter{0};
    return prefix + std::to_string(counter.fetch_add(1)) + "@example.test";
}

/// Returns a value, so a range-for over a member must bind the body to a local first:
/// iterating `bodyOf(response)["items"]` directly walks a destroyed temporary.
Json::Value bodyOf(const drogon::HttpResponsePtr& response) {
    EXPECT_NE(response, nullptr);
    const auto json = response->getJsonObject();
    EXPECT_NE(json, nullptr) << "expected a JSON body";
    return json == nullptr ? Json::Value{} : *json;
}

std::string tokenOf(const Json::Value& session) {
    return session["accessToken"].asString();
}

/// A session that holds the `dev` role, i.e. one that may publish.
Json::Value developer() {
    return harness().createSessionWithRole(uniqueEmail("dev"), "dev");
}

Json::Value player() {
    return harness().createVerifiedSession(uniqueEmail("player"));
}

Json::Value gamePayload(const std::string& title, const std::string& visibility = "draft") {
    Json::Value body;
    body["title"] = title;
    body["summary"] = "A summary";
    body["visibility"] = visibility;
    return body;
}

/// Creates a game and returns its representation.
Json::Value createGame(const Json::Value& session,
                       const std::string& title,
                       const std::string& visibility = "draft") {
    const auto response =
        harness().postJson("/api/v1/games", gamePayload(title, visibility), tokenOf(session));
    EXPECT_EQ(response->statusCode(), drogon::k201Created) << response->body();
    return bodyOf(response);
}

// ---------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------

TEST(CatalogEndpointTest, CreatesAGameAndDerivesItsSlug) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();

    const auto game = createGame(session, "The Long Dusk");

    EXPECT_EQ(game["slug"].asString(), "the-long-dusk");
    EXPECT_EQ(game["visibility"].asString(), "draft");
    EXPECT_FALSE(game["publisher"]["displayName"].asString().empty());
}

// The devlist is the whole authorization model for publishing: a plain account must not be
// able to create a catalog entry, whatever its client shows.
TEST(CatalogEndpointTest, RefusesToPublishWithoutTheDevRole) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = player();

    const auto response =
        harness().postJson("/api/v1/games", gamePayload("Not Mine"), tokenOf(session));

    EXPECT_EQ(response->statusCode(), drogon::k403Forbidden);
    EXPECT_EQ(bodyOf(response)["code"].asString(), "forbidden");
}

TEST(CatalogEndpointTest, RejectsASlugThatIsAlreadyTaken) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    createGame(session, "Duplicate Slug Game");

    const auto response =
        harness().postJson("/api/v1/games", gamePayload("Duplicate Slug Game"), tokenOf(session));

    EXPECT_EQ(response->statusCode(), drogon::k409Conflict);
    EXPECT_EQ(bodyOf(response)["code"].asString(), "conflict");
}

TEST(CatalogEndpointTest, RejectsAMalformedReleaseDate) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();

    auto payload = gamePayload("Badly Dated Game");
    payload["releaseDate"] = "31-12-2026";
    const auto response = harness().postJson("/api/v1/games", payload, tokenOf(session));

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
    EXPECT_NE(bodyOf(response)["detail"].asString().find("releaseDate"), std::string::npos);
}

TEST(CatalogEndpointTest, UpdatesOnlyTheFieldsThePatchNames) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Partially Patched");

    Json::Value patch;
    patch["title"] = "Renamed";
    const auto response =
        harness().patchJson("/api/v1/games/" + game["id"].asString(), patch, tokenOf(session));

    ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
    EXPECT_EQ(bodyOf(response)["title"].asString(), "Renamed");
    EXPECT_EQ(bodyOf(response)["summary"].asString(), "A summary")
        << "an omitted field must not be blanked";
}

TEST(CatalogEndpointTest, RefusesToEditAnotherPublishersGame) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto owner = developer();
    const auto game = createGame(owner, "Somebody Elses Game", "public");
    const auto intruder = developer();

    Json::Value patch;
    patch["title"] = "Hijacked";
    const auto response =
        harness().patchJson("/api/v1/games/" + game["id"].asString(), patch, tokenOf(intruder));

    EXPECT_EQ(response->statusCode(), drogon::k403Forbidden);
}

// ---------------------------------------------------------------------------
// Explore and visibility
// ---------------------------------------------------------------------------

TEST(CatalogEndpointTest, ExploreListsPublishedGamesAndHidesDrafts) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto listed = createGame(session, "Explore Listed Title", "public");
    const auto hidden = createGame(session, "Explore Hidden Title", "draft");

    const auto response =
        harness().get("/api/v1/games?search=Explore&pageSize=50", tokenOf(player()));

    ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
    const auto body = bodyOf(response);

    bool sawListed = false;
    bool sawHidden = false;
    for (const auto& item : body["items"]) {
        sawListed = sawListed || item["id"].asString() == listed["id"].asString();
        sawHidden = sawHidden || item["id"].asString() == hidden["id"].asString();
    }
    EXPECT_TRUE(sawListed);
    EXPECT_FALSE(sawHidden) << "a draft must never appear in Explore";
}

TEST(CatalogEndpointTest, ExploreSearchesByTitle) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    createGame(session, "Zorblax Adventures", "public");

    const auto response = harness().get("/api/v1/games?search=Zorblax", tokenOf(player()));

    ASSERT_EQ(response->statusCode(), drogon::k200OK);
    const auto body = bodyOf(response);
    ASSERT_GE(body["total"].asInt64(), 1);
    EXPECT_NE(body["items"][0]["title"].asString().find("Zorblax"), std::string::npos);
}

TEST(CatalogEndpointTest, ExplorePaginates) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    createGame(session, "Paged Alpha Title", "public");
    createGame(session, "Paged Beta Title", "public");

    const auto first =
        harness().get("/api/v1/games?search=Paged&pageSize=1&page=1", tokenOf(player()));
    const auto second =
        harness().get("/api/v1/games?search=Paged&pageSize=1&page=2", tokenOf(player()));

    ASSERT_EQ(first->statusCode(), drogon::k200OK);
    ASSERT_EQ(second->statusCode(), drogon::k200OK);
    EXPECT_EQ(bodyOf(first)["items"].size(), 1U);
    EXPECT_EQ(bodyOf(second)["items"].size(), 1U);
    EXPECT_GE(bodyOf(first)["total"].asInt64(), 2)
        << "the total must count the whole result, not the page";
    EXPECT_NE(bodyOf(first)["items"][0]["id"].asString(),
              bodyOf(second)["items"][0]["id"].asString());
}

TEST(CatalogEndpointTest, ResolvesAGameByEitherIdOrSlug) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Addressable Title", "public");

    const auto byId = harness().get("/api/v1/games/" + game["id"].asString(), tokenOf(session));
    const auto bySlug = harness().get("/api/v1/games/" + game["slug"].asString(), tokenOf(session));

    ASSERT_EQ(byId->statusCode(), drogon::k200OK);
    ASSERT_EQ(bySlug->statusCode(), drogon::k200OK);
    EXPECT_EQ(bodyOf(byId)["game"]["id"].asString(), bodyOf(bySlug)["game"]["id"].asString());
}

// Reporting Forbidden would confirm the unreleased title exists, so it is reported absent.
TEST(CatalogEndpointTest, ADraftIsNotFoundForAnybodyButItsPublisher) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto owner = developer();
    const auto draft = createGame(owner, "Unannounced Title");

    const auto stranger =
        harness().get("/api/v1/games/" + draft["id"].asString(), tokenOf(player()));
    const auto publisher = harness().get("/api/v1/games/" + draft["id"].asString(), tokenOf(owner));

    EXPECT_EQ(stranger->statusCode(), drogon::k404NotFound);
    EXPECT_EQ(publisher->statusCode(), drogon::k200OK);
}

TEST(CatalogEndpointTest, TheDashboardShowsAPublishersOwnDrafts) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto draft = createGame(session, "Dashboard Draft Title");

    const auto response = harness().get("/api/v1/me/games?pageSize=50", tokenOf(session));

    ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
    const auto body = bodyOf(response);
    bool found = false;
    for (const auto& item : body["items"]) {
        found = found || item["id"].asString() == draft["id"].asString();
    }
    EXPECT_TRUE(found) << response->body();
}

TEST(CatalogEndpointTest, EveryCatalogRouteRequiresACredential) {
    LAUNCHER_REQUIRE_DATABASE();

    EXPECT_EQ(harness().get("/api/v1/games")->statusCode(), drogon::k401Unauthorized);
    EXPECT_EQ(harness().get("/api/v1/library")->statusCode(), drogon::k401Unauthorized);
    EXPECT_EQ(harness().postJson("/api/v1/games", gamePayload("No Token"))->statusCode(),
              drogon::k401Unauthorized);
}

// ---------------------------------------------------------------------------
// Versions and builds
// ---------------------------------------------------------------------------

TEST(CatalogEndpointTest, CreatesAVersionAndABuildForIt) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Versioned Title");

    Json::Value version;
    version["semver"] = "0.2.1";
    version["stage"] = "beta";
    version["releaseNotes"] = "First playable";
    const auto created = harness().postJson(
        "/api/v1/games/" + game["id"].asString() + "/versions", version, tokenOf(session));

    ASSERT_EQ(created->statusCode(), drogon::k201Created) << created->body();
    EXPECT_EQ(bodyOf(created)["semver"].asString(), "0.2.1");
    EXPECT_EQ(bodyOf(created)["stage"].asString(), "beta");
    EXPECT_FALSE(bodyOf(created)["published"].asBool());

    Json::Value build;
    build["platform"] = "windows";
    const auto builtFor =
        harness().postJson("/api/v1/games/" + game["id"].asString() + "/versions/" +
                               bodyOf(created)["id"].asString() + "/builds",
                           build,
                           tokenOf(session));

    ASSERT_EQ(builtFor->statusCode(), drogon::k201Created) << builtFor->body();
    EXPECT_EQ(bodyOf(builtFor)["platform"].asString(), "windows");
    EXPECT_EQ(bodyOf(builtFor)["architecture"].asString(), "x64");
    EXPECT_EQ(bodyOf(builtFor)["status"].asString(), "uploading");
}

// The route the maintainer went looking for and could not find, because it did not exist: a
// version created with "publish now" unticked had no way back short of deleting it.
TEST(CatalogEndpointTest, PublishesAVersionAfterTheFact) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Published Later Title");

    Json::Value version;
    version["semver"] = "0.3.0";
    const auto created = harness().postJson(
        "/api/v1/games/" + game["id"].asString() + "/versions", version, tokenOf(session));
    ASSERT_EQ(created->statusCode(), drogon::k201Created) << created->body();
    ASSERT_FALSE(bodyOf(created)["published"].asBool());

    const std::string path =
        "/api/v1/games/" + game["id"].asString() + "/versions/" + bodyOf(created)["id"].asString();

    Json::Value publish;
    publish["published"] = true;
    const auto published = harness().patchJson(path, publish, tokenOf(session));

    ASSERT_EQ(published->statusCode(), drogon::k200OK) << published->body();
    EXPECT_TRUE(bodyOf(published)["published"].asBool());
    const auto firstPublishedAt = bodyOf(published)["publishedAt"].asString();
    EXPECT_FALSE(firstPublishedAt.empty());

    // Pressing it again must not move the date the release went out.
    const auto again = harness().patchJson(path, publish, tokenOf(session));
    ASSERT_EQ(again->statusCode(), drogon::k200OK) << again->body();
    EXPECT_EQ(bodyOf(again)["publishedAt"].asString(), firstPublishedAt);

    // And back: withdrawing is the reversible thing next to a delete that is not.
    Json::Value withdraw;
    withdraw["published"] = false;
    const auto withdrawn = harness().patchJson(path, withdraw, tokenOf(session));
    ASSERT_EQ(withdrawn->statusCode(), drogon::k200OK) << withdrawn->body();
    EXPECT_FALSE(bodyOf(withdrawn)["published"].asBool());
}

TEST(CatalogEndpointTest, AVersionPatchLeavesTheFieldsItDoesNotName) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Partially Patched Title");

    Json::Value version;
    version["semver"] = "1.1.0";
    version["stage"] = "beta";
    version["publish"] = true;
    const auto created = harness().postJson(
        "/api/v1/games/" + game["id"].asString() + "/versions", version, tokenOf(session));
    ASSERT_EQ(created->statusCode(), drogon::k201Created) << created->body();

    Json::Value patch;
    patch["releaseNotes"] = "now with fewer crashes";
    const auto updated = harness().patchJson("/api/v1/games/" + game["id"].asString() +
                                                 "/versions/" + bodyOf(created)["id"].asString(),
                                             patch,
                                             tokenOf(session));

    ASSERT_EQ(updated->statusCode(), drogon::k200OK) << updated->body();
    EXPECT_EQ(bodyOf(updated)["releaseNotes"].asString(), "now with fewer crashes");
    EXPECT_EQ(bodyOf(updated)["stage"].asString(), "beta");
    EXPECT_TRUE(bodyOf(updated)["published"].asBool()) << "it must still be published";
}

TEST(CatalogEndpointTest, RefusesToPatchAnotherPublishersVersion) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto owner = developer();
    const auto game = createGame(owner, "Somebody Elses Version");

    Json::Value version;
    version["semver"] = "1.0.0";
    const auto created = harness().postJson(
        "/api/v1/games/" + game["id"].asString() + "/versions", version, tokenOf(owner));
    ASSERT_EQ(created->statusCode(), drogon::k201Created);

    const auto intruder = developer();
    Json::Value publish;
    publish["published"] = true;
    const auto refused = harness().patchJson("/api/v1/games/" + game["id"].asString() +
                                                 "/versions/" + bodyOf(created)["id"].asString(),
                                             publish,
                                             tokenOf(intruder));

    EXPECT_EQ(refused->statusCode(), drogon::k404NotFound) << refused->body();
}

// A build's identity is (version, platform, architecture), which is exactly what a publisher
// looking at a list of identical rows cannot use to tell them apart.
TEST(CatalogEndpointTest, ABuildKeepsTheNameItWasGiven) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Named Build Title");

    Json::Value version;
    version["semver"] = "2.0.0";
    const auto created = harness().postJson(
        "/api/v1/games/" + game["id"].asString() + "/versions", version, tokenOf(session));
    ASSERT_EQ(created->statusCode(), drogon::k201Created);

    Json::Value build;
    build["platform"] = "windows";
    build["name"] = "Nightly, with the demo levels";
    const auto builtFor =
        harness().postJson("/api/v1/games/" + game["id"].asString() + "/versions/" +
                               bodyOf(created)["id"].asString() + "/builds",
                           build,
                           tokenOf(session));

    ASSERT_EQ(builtFor->statusCode(), drogon::k201Created) << builtFor->body();
    EXPECT_EQ(bodyOf(builtFor)["name"].asString(), "Nightly, with the demo levels");

    // And it survives to the detail response, which is where the dashboard reads it from.
    const auto detail = harness().get("/api/v1/games/" + game["id"].asString(), tokenOf(session));
    ASSERT_EQ(detail->statusCode(), drogon::k200OK);
    const Json::Value body = bodyOf(detail);
    ASSERT_EQ(body["builds"].size(), 1U);
    EXPECT_EQ(body["builds"][0]["name"].asString(), "Nightly, with the demo levels");
}

// A build published before migration 0006, and one published by a client that sends no name,
// are the same thing from here: the column defaults to empty and stays valid.
TEST(CatalogEndpointTest, ABuildWithoutANameIsStillABuild) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Unnamed Build Title");

    Json::Value version;
    version["semver"] = "2.0.0";
    const auto created = harness().postJson(
        "/api/v1/games/" + game["id"].asString() + "/versions", version, tokenOf(session));
    ASSERT_EQ(created->statusCode(), drogon::k201Created);

    Json::Value build;
    build["platform"] = "linux";
    const auto builtFor =
        harness().postJson("/api/v1/games/" + game["id"].asString() + "/versions/" +
                               bodyOf(created)["id"].asString() + "/builds",
                           build,
                           tokenOf(session));

    ASSERT_EQ(builtFor->statusCode(), drogon::k201Created) << builtFor->body();
    EXPECT_EQ(bodyOf(builtFor)["name"].asString(), "");
}

TEST(CatalogEndpointTest, RejectsAMalformedVersionNumber) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Badly Versioned Title");

    Json::Value version;
    version["semver"] = "v1.0-beta";
    const auto response = harness().postJson(
        "/api/v1/games/" + game["id"].asString() + "/versions", version, tokenOf(session));

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
}

TEST(CatalogEndpointTest, RejectsASecondBuildForTheSamePlatform) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Twice Built Title");

    Json::Value version;
    version["semver"] = "1.0";
    const auto created = harness().postJson(
        "/api/v1/games/" + game["id"].asString() + "/versions", version, tokenOf(session));
    ASSERT_EQ(created->statusCode(), drogon::k201Created);

    const auto path = "/api/v1/games/" + game["id"].asString() + "/versions/" +
                      bodyOf(created)["id"].asString() + "/builds";
    Json::Value build;
    build["platform"] = "linux";
    ASSERT_EQ(harness().postJson(path, build, tokenOf(session))->statusCode(), drogon::k201Created);

    EXPECT_EQ(harness().postJson(path, build, tokenOf(session))->statusCode(),
              drogon::k409Conflict);
}

// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------

TEST(CatalogEndpointTest, AddsAndRemovesAGameFromTheLibrary) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto owner = developer();
    const auto game = createGame(owner, "Collectable Title", "public");
    const auto session = player();

    ASSERT_EQ(
        harness().put("/api/v1/library/" + game["id"].asString(), tokenOf(session))->statusCode(),
        drogon::k200OK);
    // Adding twice is not an error; the endpoint is idempotent by design.
    ASSERT_EQ(
        harness().put("/api/v1/library/" + game["id"].asString(), tokenOf(session))->statusCode(),
        drogon::k200OK);

    const auto listed = harness().get("/api/v1/library", tokenOf(session));
    ASSERT_EQ(listed->statusCode(), drogon::k200OK);
    ASSERT_EQ(bodyOf(listed)["items"].size(), 1U);
    EXPECT_EQ(bodyOf(listed)["items"][0]["id"].asString(), game["id"].asString());

    ASSERT_EQ(harness()
                  .remove("/api/v1/library/" + game["id"].asString(), tokenOf(session))
                  ->statusCode(),
              drogon::k200OK);
    EXPECT_EQ(bodyOf(harness().get("/api/v1/library", tokenOf(session)))["items"].size(), 0U);
}

TEST(CatalogEndpointTest, RemovingAGameThatIsNotInTheLibraryIsNotFound) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto owner = developer();
    const auto game = createGame(owner, "Never Collected Title", "public");

    const auto response =
        harness().remove("/api/v1/library/" + game["id"].asString(), tokenOf(player()));

    EXPECT_EQ(response->statusCode(), drogon::k404NotFound);
}

// The library must not become a way to confirm that an unannounced title exists.
TEST(CatalogEndpointTest, RefusesToAddADraftToSomebodyElsesLibrary) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto owner = developer();
    const auto draft = createGame(owner, "Secret Collectable Title");

    const auto response =
        harness().put("/api/v1/library/" + draft["id"].asString(), tokenOf(player()));

    EXPECT_EQ(response->statusCode(), drogon::k404NotFound);
}

TEST(CatalogEndpointTest, TheLibraryShowsTheGameDetailAsAlreadyOwned) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto owner = developer();
    const auto game = createGame(owner, "Owned Flag Title", "public");
    const auto session = player();

    const auto before = harness().get("/api/v1/games/" + game["id"].asString(), tokenOf(session));
    ASSERT_EQ(before->statusCode(), drogon::k200OK);
    EXPECT_FALSE(bodyOf(before)["inLibrary"].asBool());

    ASSERT_EQ(
        harness().put("/api/v1/library/" + game["id"].asString(), tokenOf(session))->statusCode(),
        drogon::k200OK);

    const auto after = harness().get("/api/v1/games/" + game["id"].asString(), tokenOf(session));
    EXPECT_TRUE(bodyOf(after)["inLibrary"].asBool());
}

// ---------------------------------------------------------------------------
// Adding to a game somebody else publishes
//
// The dashboard used to show one account the previous account's game, and the question that
// followed was whether the buttons on it would have worked. Driving the real server with two
// publishers answered it — every write route refuses — but two of them were refusing with
// nothing asserting that they do: **creating** a version, and **creating** a build. Editing a
// game and patching a version are covered above, and the three deletes live in
// `RetentionEndpointTest` (`RefusesToDelete…`), which is where a delete belongs. §9.5, and D62
// was found in this exact gap for reads.
//
// The game here is **public** on purpose. On a draft the intruder cannot see it at all and the
// answer is 404 through `mayViewGame` (D30), which says nothing about whether ownership is
// checked; on a public game the refusal has to come from `mayEditGame`.
// ---------------------------------------------------------------------------

/// A public game owned by somebody else, with a published version on it.
struct OtherPublishersGame {
    Json::Value owner;
    std::string gameId;
    std::string versionId;
};

OtherPublishersGame otherPublishersGame(const std::string& title) {
    OtherPublishersGame theirs;
    theirs.owner = developer();
    theirs.gameId = createGame(theirs.owner, title, "public")["id"].asString();

    Json::Value version;
    version["semver"] = "1.0.0";
    version["publish"] = true;
    const auto created = harness().postJson(
        "/api/v1/games/" + theirs.gameId + "/versions", version, tokenOf(theirs.owner));
    EXPECT_EQ(created->statusCode(), drogon::k201Created) << created->body();
    theirs.versionId = bodyOf(created)["id"].asString();
    return theirs;
}

TEST(CatalogEndpointTest, RefusesToAddAVersionToAnotherPublishersGame) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto theirs = otherPublishersGame("No Versions From Strangers");
    const auto intruder = developer();

    Json::Value version;
    version["semver"] = "9.9.9";
    const auto refused = harness().postJson(
        "/api/v1/games/" + theirs.gameId + "/versions", version, tokenOf(intruder));

    EXPECT_EQ(refused->statusCode(), drogon::k403Forbidden) << refused->body();
}

TEST(CatalogEndpointTest, RefusesToAddABuildToAnotherPublishersVersion) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto theirs = otherPublishersGame("No Builds From Strangers");
    const auto intruder = developer();

    Json::Value build;
    build["platform"] = "linux";
    const auto refused = harness().postJson("/api/v1/games/" + theirs.gameId + "/versions/" +
                                                theirs.versionId + "/builds",
                                            build,
                                            tokenOf(intruder));

    EXPECT_EQ(refused->statusCode(), drogon::k403Forbidden) << refused->body();
}

} // namespace
