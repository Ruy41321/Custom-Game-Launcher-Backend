#include <gtest/gtest.h>

#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
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

/// An ISO base media header with an ordinary MP4 brand, padded so the size checks have
/// something to measure. Nothing here is a playable video: the server identifies a container
/// and never claims to have decoded one.
std::string mp4Body(const std::string& suffix = "one", std::size_t size = 64) {
    auto body = std::string("QQQQ", 4) + "ftypisom" + suffix;
    body[0] = 0;
    body[1] = 0;
    body[2] = 0;
    body[3] = ' ';
    body.resize(std::max(size, body.size()), 'v');
    return body;
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

TEST(MediaEndpointTest, StoresAVideoAndReturnsAPublicUrlItsExtensionMakesPlayable) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Trailer");

    const auto response = uploadMedia(session, game["id"].asString(), "video", mp4Body());

    ASSERT_EQ(response->statusCode(), drogon::k201Created) << response->body();
    const auto media = bodyOf(response);
    EXPECT_EQ(media["kind"].asString(), "video");
    EXPECT_EQ(media["contentType"].asString(), "video/mp4");
    // The extension is in the URL because it is in the storage key, and the file server's
    // regex location only routes the five it knows. Serving a trailer as
    // application/octet-stream is serving a download nobody can play.
    const auto url = media["url"].asString();
    EXPECT_EQ(url.substr(url.size() - 4), ".mp4");
    EXPECT_EQ(url.find('?'), std::string::npos);
    EXPECT_TRUE(std::filesystem::is_regular_file(storedFileFor(media)));
}

/// The two limits are one number apart in the harness on purpose: the same body is too large
/// for a picture and small enough for a video, so this is the kind choosing the budget rather
/// than a body that happens to fit.
TEST(MediaEndpointTest, MeasuresAVideoAgainstItsOwnLimit) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Big Trailer");

    const auto asAPicture = uploadMedia(
        session, game["id"].asString(), "screenshot", PNG_HEADER + std::string(8192, 'x'));
    EXPECT_EQ(asAPicture->statusCode(), drogon::k422UnprocessableEntity) << asAPicture->body();

    const auto accepted = uploadMedia(session, game["id"].asString(), "video", mp4Body("a", 8192));
    EXPECT_EQ(accepted->statusCode(), drogon::k201Created) << accepted->body();

    const auto refused = uploadMedia(session, game["id"].asString(), "video", mp4Body("b", 20000));
    EXPECT_EQ(refused->statusCode(), drogon::k422UnprocessableEntity) << refused->body();
}

TEST(MediaEndpointTest, RefusesAPictureSentAsAVideoAndAVideoSentAsAPicture) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Mismatched Kinds");

    const auto picture = uploadMedia(session, game["id"].asString(), "video", pngBody());
    EXPECT_EQ(picture->statusCode(), drogon::k422UnprocessableEntity) << picture->body();

    const auto video = uploadMedia(session, game["id"].asString(), "screenshot", mp4Body());
    EXPECT_EQ(video->statusCode(), drogon::k422UnprocessableEntity) << video->body();
}

/// The partial unique index changed shape in migration 0008 — from "everything but a
/// screenshot is singular" to a list of the three kinds that are — and this is what would break
/// if it had not: the second video would replace the first instead of joining it.
TEST(MediaEndpointTest, VideosAreAGalleryAndNotASingleton) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = developer();
    const auto game = createGame(session, "Two Clips");

    ASSERT_EQ(uploadMedia(session, game["id"].asString(), "video", mp4Body("first"))->statusCode(),
              drogon::k201Created);
    ASSERT_EQ(uploadMedia(session, game["id"].asString(), "video", mp4Body("second"))->statusCode(),
              drogon::k201Created);

    const auto listing = bodyOf(
        harness().get("/api/v1/games/" + game["id"].asString() + "/media", tokenOf(session)));

    int videos = 0;
    for (const auto& item : listing["items"]) {
        if (item["kind"].asString() == "video") {
            ++videos;
        }
    }
    EXPECT_EQ(videos, 2);
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

// ---------------------------------------------------------------------------
// Artwork on a game somebody else publishes
//
// The one case already covered is a *draft*, where the refusal comes from not being able to
// see the game at all. On a public game the refusal has to come from `mayEditGame` (D30), and
// nothing asserted that for uploading, editing or removing a picture — §9.5.
// ---------------------------------------------------------------------------

TEST(MediaEndpointTest, RefusesToUploadArtworkToAnotherPublishersGame) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto owner = developer();
    const auto game = createGame(owner, "Guarded Artwork Title");
    const auto intruder = developer();

    const auto refused =
        uploadMedia(intruder, game["id"].asString(), "screenshot", pngBody("smuggled"));

    EXPECT_EQ(refused->statusCode(), drogon::k403Forbidden) << refused->body();
}

TEST(MediaEndpointTest, RefusesToEditAnotherPublishersArtwork) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto owner = developer();
    const auto game = createGame(owner, "Guarded Alt Text Title");
    const auto uploaded = uploadMedia(owner, game["id"].asString(), "cover", pngBody("theirs"));
    ASSERT_EQ(uploaded->statusCode(), drogon::k201Created) << uploaded->body();
    const auto intruder = developer();

    Json::Value patch;
    patch["altText"] = "hijacked";
    const auto refused = harness().patchJson(
        "/api/v1/media/" + bodyOf(uploaded)["id"].asString(), patch, tokenOf(intruder));

    EXPECT_EQ(refused->statusCode(), drogon::k403Forbidden) << refused->body();
}

TEST(MediaEndpointTest, RefusesToRemoveAnotherPublishersArtwork) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto owner = developer();
    const auto game = createGame(owner, "Guarded Removal Title");
    const auto uploaded = uploadMedia(owner, game["id"].asString(), "cover", pngBody("keep"));
    ASSERT_EQ(uploaded->statusCode(), drogon::k201Created) << uploaded->body();
    const auto intruder = developer();

    const auto refused =
        harness().remove("/api/v1/media/" + bodyOf(uploaded)["id"].asString(), tokenOf(intruder));

    EXPECT_EQ(refused->statusCode(), drogon::k403Forbidden) << refused->body();
    EXPECT_TRUE(std::filesystem::exists(storedFileFor(bodyOf(uploaded))))
        << "the picture must still be on disk";
}

} // namespace
