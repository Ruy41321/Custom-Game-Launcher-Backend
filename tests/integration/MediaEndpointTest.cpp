#include <gtest/gtest.h>

#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <atomic>
#include <filesystem>
#include <map>
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

/// Returns a value: iterating `bodyOf(response)["items"]` directly walks a destroyed
/// temporary, which is the trap recorded in CLAUDE.md section 8.
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
    return harness().createSessionWithRole(uniqueEmail("mediadev"), "dev");
}

Json::Value player() {
    return harness().createVerifiedSession(uniqueEmail("mediaplayer"));
}

const std::string PNG_HEADER("\x89PNG\r\n\x1a\n", 8);

/// A body that sniffs as a PNG. The suffix changes the content address, which is what lets a
/// test tell one picture from another.
std::string pngBody(const std::string& suffix = "one") {
    return PNG_HEADER + suffix;
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

drogon::HttpResponsePtr uploadMedia(const Json::Value& session,
                                    const std::string& gameId,
                                    const std::string& kind,
                                    const std::string& body,
                                    const std::map<std::string, std::string>& extra = {}) {
    std::map<std::string, std::string> parameters{{"kind", kind}};
    for (const auto& [name, value] : extra) {
        parameters[name] = value;
    }
    return harness().postBinary(
        "/api/v1/games/" + gameId + "/media", parameters, body, tokenOf(session));
}

/// The path the file server would serve, resolved against the harness media root.
std::filesystem::path storedFileFor(const Json::Value& media) {
    const auto url = media["url"].asString();
    const std::string base = "http://files.test/media/";
    EXPECT_EQ(url.rfind(base, 0), 0u) << "unexpected media URL: " << url;
    return harness().mediaRoot() / url.substr(base.size());
}

// ---------------------------------------------------------------------------
// Uploading
// ---------------------------------------------------------------------------

TEST(MediaEndpointTest, StoresACoverAndReturnsAPublicUrl) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Cover Art");

    const auto response = uploadMedia(session, game["id"].asString(), "cover", pngBody());

    ASSERT_EQ(response->statusCode(), drogon::k201Created) << response->body();
    const auto media = bodyOf(response);
    EXPECT_EQ(media["kind"].asString(), "cover");
    EXPECT_EQ(media["contentType"].asString(), "image/png");
    EXPECT_EQ(media["sizeBytes"].asInt64(), static_cast<Json::Int64>(pngBody().size()));
    // No token and no expiry anywhere in it: artwork is served unsigned, which is the whole
    // difference between this root and the blob root.
    EXPECT_EQ(media["url"].asString().find('?'), std::string::npos);
    EXPECT_TRUE(std::filesystem::is_regular_file(storedFileFor(media)));
}

TEST(MediaEndpointTest, RefusesABodyThatIsNotAnImage) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Not An Image");

    const auto response =
        uploadMedia(session, game["id"].asString(), "cover", "<svg><script/></svg>");

    // The request announced application/octet-stream and the server never looked: an SVG on a
    // public URL is a stored script, not a picture.
    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity) << response->body();
}

TEST(MediaEndpointTest, RefusesAnImagePastTheConfiguredLimit) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Too Large");

    const auto response =
        uploadMedia(session, game["id"].asString(), "cover", PNG_HEADER + std::string(8192, 'x'));

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity) << response->body();
}

TEST(MediaEndpointTest, RefusesAKindItDoesNotKnow) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Bad Kind");

    const auto response = uploadMedia(session, game["id"].asString(), "thumbnail", pngBody());

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity) << response->body();
}

TEST(MediaEndpointTest, ASecondCoverReplacesTheFirst) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Replaced Cover");
    const auto first =
        bodyOf(uploadMedia(session, game["id"].asString(), "cover", pngBody("first")));

    const auto second =
        bodyOf(uploadMedia(session, game["id"].asString(), "cover", pngBody("second")));

    EXPECT_NE(second["url"].asString(), first["url"].asString());

    const auto listing = bodyOf(
        harness().get("/api/v1/games/" + game["id"].asString() + "/media", tokenOf(session)));
    ASSERT_EQ(listing["items"].size(), 1u) << "a game has exactly one cover";
    EXPECT_EQ(listing["items"][0]["url"].asString(), second["url"].asString());
    EXPECT_FALSE(std::filesystem::is_regular_file(storedFileFor(first)))
        << "the displaced picture is unreferenced and should not survive on disk";
}

TEST(MediaEndpointTest, TwoGamesMayShareOnePicture) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto first = createGame(session, "Shared Art One");
    const auto second = createGame(session, "Shared Art Two");

    const auto one =
        bodyOf(uploadMedia(session, first["id"].asString(), "cover", pngBody("shared")));
    const auto two =
        bodyOf(uploadMedia(session, second["id"].asString(), "cover", pngBody("shared")));

    // Content-addressed: the same bytes are the same file, uploaded once.
    EXPECT_EQ(one["url"].asString(), two["url"].asString());

    ASSERT_EQ(
        harness().remove("/api/v1/media/" + one["id"].asString(), tokenOf(session))->statusCode(),
        drogon::k204NoContent);
    EXPECT_TRUE(std::filesystem::is_regular_file(storedFileFor(two)))
        << "deleting one game's row must not blank the other game's cover";
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

TEST(MediaEndpointTest, TheGameDetailCarriesTheArtworkAndTheCoverUrl) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Detailed Art");
    const auto cover = bodyOf(uploadMedia(session, game["id"].asString(), "cover", pngBody("c")));
    ASSERT_EQ(uploadMedia(session, game["id"].asString(), "screenshot", pngBody("s"))->statusCode(),
              drogon::k201Created);

    const auto detail =
        bodyOf(harness().get("/api/v1/games/" + game["id"].asString(), tokenOf(session)));

    EXPECT_EQ(detail["game"]["coverUrl"].asString(), cover["url"].asString());
    EXPECT_EQ(detail["media"].size(), 2u);
}

TEST(MediaEndpointTest, ExploreCarriesACoverUrlPerCard) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Explorable Art");
    const auto cover = bodyOf(uploadMedia(session, game["id"].asString(), "cover", pngBody("e")));

    const auto page =
        bodyOf(harness().get("/api/v1/games?search=Explorable Art", tokenOf(session)));

    const auto items = page["items"];
    ASSERT_GE(items.size(), 1u);
    bool found = false;
    for (const auto& item : items) {
        if (item["id"].asString() == game["id"].asString()) {
            found = true;
            // One picture per card is the whole reason the cover rides on the game itself
            // rather than needing a second request per result.
            EXPECT_EQ(item["coverUrl"].asString(), cover["url"].asString());
        }
    }
    EXPECT_TRUE(found);
}

TEST(MediaEndpointTest, AGameWithNoCoverReportsAnEmptyUrlRatherThanOmittingIt) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "No Art At All");

    const auto detail =
        bodyOf(harness().get("/api/v1/games/" + game["id"].asString(), tokenOf(session)));

    ASSERT_TRUE(detail["game"].isMember("coverUrl"));
    EXPECT_TRUE(detail["game"]["coverUrl"].asString().empty());
    EXPECT_EQ(detail["media"].size(), 0u);
}

// ---------------------------------------------------------------------------
// Authorization
// ---------------------------------------------------------------------------

TEST(MediaEndpointTest, APlayerCannotUploadArtwork) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Not Yours");

    const auto response = uploadMedia(player(), game["id"].asString(), "cover", pngBody());

    EXPECT_EQ(response->statusCode(), drogon::k403Forbidden) << response->body();
}

TEST(MediaEndpointTest, ArtworkOnSomebodyElsesDraftIsNotFoundRatherThanForbidden) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto owner = developer();
    const auto game = createGame(owner, "Secret Draft Art", "draft");
    ASSERT_EQ(uploadMedia(owner, game["id"].asString(), "cover", pngBody())->statusCode(),
              drogon::k201Created);

    const auto response =
        harness().get("/api/v1/games/" + game["id"].asString() + "/media", tokenOf(player()));

    // A 403 would confirm the unreleased title exists, which is the rule the whole catalog
    // follows.
    EXPECT_EQ(response->statusCode(), drogon::k404NotFound) << response->body();
}

TEST(MediaEndpointTest, RequiresAuthentication) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Anonymous Art");

    const auto response = harness().get("/api/v1/games/" + game["id"].asString() + "/media");

    EXPECT_EQ(response->statusCode(), drogon::k401Unauthorized) << response->body();
}

// ---------------------------------------------------------------------------
// Editing and removing
// ---------------------------------------------------------------------------

TEST(MediaEndpointTest, ChangesTheAltTextWithoutTouchingThePicture) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Described Art");
    const auto media =
        bodyOf(uploadMedia(session, game["id"].asString(), "screenshot", pngBody("d")));

    Json::Value changes;
    changes["altText"] = "A knight standing in the rain";
    changes["sortOrder"] = 2;
    const auto response =
        harness().patchJson("/api/v1/media/" + media["id"].asString(), changes, tokenOf(session));

    ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
    const auto updated = bodyOf(response);
    EXPECT_EQ(updated["altText"].asString(), "A knight standing in the rain");
    EXPECT_EQ(updated["sortOrder"].asInt(), 2);
    EXPECT_EQ(updated["url"].asString(), media["url"].asString());
}

TEST(MediaEndpointTest, RemovesArtworkAndTheFileBehindIt) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Deleted Art");
    const auto media = bodyOf(uploadMedia(session, game["id"].asString(), "cover", pngBody("x")));
    const auto file = storedFileFor(media);
    ASSERT_TRUE(std::filesystem::is_regular_file(file));

    const auto response =
        harness().remove("/api/v1/media/" + media["id"].asString(), tokenOf(session));

    ASSERT_EQ(response->statusCode(), drogon::k204NoContent) << response->body();
    EXPECT_FALSE(std::filesystem::is_regular_file(file));
    EXPECT_EQ(
        harness().remove("/api/v1/media/" + media["id"].asString(), tokenOf(session))->statusCode(),
        drogon::k404NotFound);
}

TEST(MediaEndpointTest, StoresTheAltTextGivenAtUploadTime) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Annotated Art");

    const auto media = bodyOf(uploadMedia(session,
                                          game["id"].asString(),
                                          "screenshot",
                                          pngBody("a"),
                                          {{"altText", "The title screen"}, {"sortOrder", "5"}}));

    EXPECT_EQ(media["altText"].asString(), "The title screen");
    EXPECT_EQ(media["sortOrder"].asInt(), 5);
}

} // namespace
