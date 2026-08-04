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

Json::Value bodyOf(const drogon::HttpResponsePtr& response) {
    EXPECT_NE(response, nullptr);
    const auto json = response->getJsonObject();
    EXPECT_NE(json, nullptr) << "expected a JSON body";
    return json == nullptr ? Json::Value{} : *json;
}

std::string tokenOf(const Json::Value& session) {
    return session["accessToken"].asString();
}

Json::Value developer() {
    return harness().createSessionWithRole(uniqueEmail("devlog"), "dev");
}

Json::Value createGame(const Json::Value& session,
                       const std::string& title,
                       const std::string& visibility = "public") {
    Json::Value body;
    body["title"] = title;
    body["visibility"] = visibility;
    const auto response = harness().postJson("/api/v1/games", body, tokenOf(session));
    EXPECT_EQ(response->statusCode(), drogon::k201Created) << response->body();
    return bodyOf(response);
}

Json::Value notePayload(const std::string& title, bool publish) {
    Json::Value body;
    body["title"] = title;
    body["bodyMarkdown"] = "## Changed\n\nSomething.";
    body["publish"] = publish;
    return body;
}

drogon::HttpResponsePtr postNote(const Json::Value& session,
                                 const std::string& gameId,
                                 const std::string& title,
                                 bool publish = true) {
    return harness().postJson(
        "/api/v1/games/" + gameId + "/patch-notes", notePayload(title, publish), tokenOf(session));
}

TEST(PatchNoteEndpointTest, PublishesANoteAndListsIt) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Devlogged Game");

    const auto response = postNote(session, game["id"].asString(), "Release day");

    ASSERT_EQ(response->statusCode(), drogon::k201Created) << response->body();
    const auto note = bodyOf(response);
    EXPECT_TRUE(note["published"].asBool());
    EXPECT_FALSE(note["publishedAt"].asString().empty());
    EXPECT_FALSE(note["author"]["displayName"].asString().empty());

    const auto listing = bodyOf(
        harness().get("/api/v1/games/" + game["id"].asString() + "/patch-notes", tokenOf(session)));
    ASSERT_EQ(listing["items"].size(), 1u);
    EXPECT_EQ(listing["total"].asInt64(), 1);
    EXPECT_EQ(listing["items"][0]["title"].asString(), "Release day");
}

TEST(PatchNoteEndpointTest, HidesDraftsFromEverybodyButThePublisher) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Half Written Devlog");
    ASSERT_EQ(postNote(session, game["id"].asString(), "Published", true)->statusCode(),
              drogon::k201Created);
    ASSERT_EQ(postNote(session, game["id"].asString(), "Draft", false)->statusCode(),
              drogon::k201Created);

    const auto reader = harness().createVerifiedSession(uniqueEmail("devlogreader"));
    const auto asReader = bodyOf(
        harness().get("/api/v1/games/" + game["id"].asString() + "/patch-notes", tokenOf(reader)));
    const auto asOwner = bodyOf(
        harness().get("/api/v1/games/" + game["id"].asString() + "/patch-notes", tokenOf(session)));

    ASSERT_EQ(asReader["items"].size(), 1u);
    EXPECT_EQ(asReader["items"][0]["title"].asString(), "Published");
    EXPECT_EQ(asOwner["items"].size(), 2u) << "a publisher sees their own drafts";
}

TEST(PatchNoteEndpointTest, Paginates) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Long Devlog");
    for (int index = 0; index < 3; ++index) {
        ASSERT_EQ(postNote(session, game["id"].asString(), "Entry " + std::to_string(index))
                      ->statusCode(),
                  drogon::k201Created);
    }

    const auto first = bodyOf(
        harness().get("/api/v1/games/" + game["id"].asString() + "/patch-notes?pageSize=2&page=1",
                      tokenOf(session)));
    const auto second = bodyOf(
        harness().get("/api/v1/games/" + game["id"].asString() + "/patch-notes?pageSize=2&page=2",
                      tokenOf(session)));

    EXPECT_EQ(first["items"].size(), 2u);
    EXPECT_EQ(first["total"].asInt64(), 3);
    EXPECT_EQ(second["items"].size(), 1u);
    EXPECT_EQ(second["offset"].asInt(), 2);
}

TEST(PatchNoteEndpointTest, AttachesANoteToAVersionAndRefusesAnotherGames) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Versioned Devlog");
    const auto other = createGame(session, "Other Devlog");

    Json::Value version;
    version["semver"] = "1.0.0";
    version["publish"] = true;
    const auto ours = bodyOf(harness().postJson(
        "/api/v1/games/" + game["id"].asString() + "/versions", version, tokenOf(session)));
    const auto theirs = bodyOf(harness().postJson(
        "/api/v1/games/" + other["id"].asString() + "/versions", version, tokenOf(session)));

    auto payload = notePayload("For 1.0.0", true);
    payload["versionId"] = ours["id"].asString();
    const auto attached = harness().postJson(
        "/api/v1/games/" + game["id"].asString() + "/patch-notes", payload, tokenOf(session));
    ASSERT_EQ(attached->statusCode(), drogon::k201Created) << attached->body();
    EXPECT_EQ(bodyOf(attached)["versionId"].asString(), ours["id"].asString());

    payload["versionId"] = theirs["id"].asString();
    const auto mismatched = harness().postJson(
        "/api/v1/games/" + game["id"].asString() + "/patch-notes", payload, tokenOf(session));

    // The foreign key would happily accept it; the mismatch would only ever surface as a note
    // pointing at another game's version.
    EXPECT_EQ(mismatched->statusCode(), drogon::k404NotFound) << mismatched->body();
}

TEST(PatchNoteEndpointTest, APlayerCannotWriteADevlog) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Read Only Devlog");
    const auto player = harness().createVerifiedSession(uniqueEmail("devlogplayer"));

    const auto response = postNote(player, game["id"].asString(), "Not mine");

    EXPECT_EQ(response->statusCode(), drogon::k403Forbidden) << response->body();
}

TEST(PatchNoteEndpointTest, TheDevlogOfADraftGameIsNotFound) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto owner = developer();
    const auto game = createGame(owner, "Unreleased Devlog", "draft");
    ASSERT_EQ(postNote(owner, game["id"].asString(), "Secret")->statusCode(), drogon::k201Created);
    const auto player = harness().createVerifiedSession(uniqueEmail("devlogplayer"));

    const auto response =
        harness().get("/api/v1/games/" + game["id"].asString() + "/patch-notes", tokenOf(player));

    EXPECT_EQ(response->statusCode(), drogon::k404NotFound) << response->body();
}

TEST(PatchNoteEndpointTest, EditsAndRetractsANote) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Edited Devlog");
    const auto note = bodyOf(postNote(session, game["id"].asString(), "First wording"));

    Json::Value changes;
    changes["title"] = "Second wording";
    changes["published"] = false;
    const auto response = harness().patchJson(
        "/api/v1/patch-notes/" + note["id"].asString(), changes, tokenOf(session));

    ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
    const auto updated = bodyOf(response);
    EXPECT_EQ(updated["title"].asString(), "Second wording");
    EXPECT_FALSE(updated["published"].asBool()) << "a note published by mistake can come back";
    EXPECT_EQ(updated["bodyMarkdown"].asString(), note["bodyMarkdown"].asString())
        << "a PATCH that omits the body must not blank it";
}

TEST(PatchNoteEndpointTest, RefusesToEditSomebodyElsesNote) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Guarded Devlog");
    const auto note = bodyOf(postNote(session, game["id"].asString(), "Mine"));
    const auto stranger = developer();

    Json::Value changes;
    changes["title"] = "Theirs";
    const auto response = harness().patchJson(
        "/api/v1/patch-notes/" + note["id"].asString(), changes, tokenOf(stranger));

    EXPECT_EQ(response->statusCode(), drogon::k403Forbidden) << response->body();
}

TEST(PatchNoteEndpointTest, RemovesANote) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Deleted Devlog");
    const auto note = bodyOf(postNote(session, game["id"].asString(), "Temporary"));

    const auto response =
        harness().remove("/api/v1/patch-notes/" + note["id"].asString(), tokenOf(session));

    ASSERT_EQ(response->statusCode(), drogon::k204NoContent) << response->body();
    EXPECT_EQ(harness()
                  .remove("/api/v1/patch-notes/" + note["id"].asString(), tokenOf(session))
                  ->statusCode(),
              drogon::k404NotFound);
}

TEST(PatchNoteEndpointTest, RequiresAuthentication) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Anonymous Devlog");

    EXPECT_EQ(
        harness().get("/api/v1/games/" + game["id"].asString() + "/patch-notes")->statusCode(),
        drogon::k401Unauthorized);
}

TEST(PatchNoteEndpointTest, RejectsANoteWithNoTitle) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Untitled Devlog");

    Json::Value body;
    body["bodyMarkdown"] = "No title at all.";
    const auto response = harness().postJson(
        "/api/v1/games/" + game["id"].asString() + "/patch-notes", body, tokenOf(session));

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity) << response->body();
}

} // namespace
