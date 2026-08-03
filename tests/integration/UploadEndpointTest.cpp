#include <gtest/gtest.h>

#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <atomic>
#include <string>

#include "common/Hash.h"
#include "integration/AppHarness.h"

namespace {

using launcher::common::sha256Hex;
using launcher::testing::AppHarness;

AppHarness& harness() {
    return *AppHarness::current();
}

std::string uniqueEmail(const std::string& prefix) {
    static std::atomic<int> counter{0};
    return prefix + std::to_string(counter.fetch_add(1)) + "@upload.test";
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

/// A build sitting in `uploading`, with the session that owns it.
struct Publication {
    Json::Value session;
    std::string gameId;
    std::string versionId;
    std::string buildId;

    std::string token() const { return tokenOf(session); }

    std::string userId() const { return session["user"]["id"].asString(); }
};

Publication newBuild(const std::string& title) {
    Publication publication;
    publication.session = harness().createSessionWithRole(uniqueEmail("publisher"), "dev");

    Json::Value game;
    game["title"] = title;
    game["visibility"] = "public";
    const auto created = harness().postJson("/api/v1/games", game, publication.token());
    EXPECT_EQ(created->statusCode(), drogon::k201Created) << created->body();
    publication.gameId = bodyOf(created)["id"].asString();

    Json::Value version;
    version["semver"] = "1.0.0";
    version["publish"] = true;
    const auto versioned = harness().postJson(
        "/api/v1/games/" + publication.gameId + "/versions", version, publication.token());
    EXPECT_EQ(versioned->statusCode(), drogon::k201Created) << versioned->body();
    publication.versionId = bodyOf(versioned)["id"].asString();

    Json::Value build;
    build["platform"] = "windows";
    const auto built = harness().postJson("/api/v1/games/" + publication.gameId + "/versions/" +
                                              publication.versionId + "/builds",
                                          build,
                                          publication.token());
    EXPECT_EQ(built->statusCode(), drogon::k201Created) << built->body();
    publication.buildId = bodyOf(built)["id"].asString();

    return publication;
}

Json::Value declaration(const std::string& content) {
    Json::Value blob;
    blob["sha256"] = sha256Hex(content);
    blob["size"] = static_cast<Json::Int64>(content.size());
    return blob;
}

drogon::HttpResponsePtr beginUpload(const Publication& publication, const std::string& content) {
    return harness().postJson("/api/v1/builds/" + publication.buildId + "/uploads",
                              declaration(content),
                              publication.token());
}

/// Uploads a whole blob in one chunk and returns the final session body.
Json::Value uploadBlob(const Publication& publication, const std::string& content) {
    const auto started = beginUpload(publication, content);
    EXPECT_EQ(started->statusCode(), drogon::k201Created) << started->body();

    const auto sessionId = bodyOf(started)["id"].asString();
    const auto sent =
        harness().patchBinary("/api/v1/uploads/" + sessionId, content, 0, publication.token());
    EXPECT_EQ(sent->statusCode(), drogon::k200OK) << sent->body();
    return bodyOf(sent);
}

Json::Value manifestPayload(const std::string& path, const std::string& content) {
    Json::Value file;
    file["path"] = path;
    file["sha256"] = sha256Hex(content);
    file["executable"] = true;

    Json::Value files(Json::arrayValue);
    files.append(file);

    Json::Value body;
    body["files"] = files;
    body["entrypoint"] = path;
    body["launchArgs"] = "--fullscreen";
    return body;
}

int64_t usedBytesFor(const std::string& userId) {
    return std::stoll(harness().database().scalar(
        "SELECT upload_used_bytes FROM users WHERE id = '" + userId + "'::uuid"));
}

// ---------------------------------------------------------------------------
// The whole publish flow
// ---------------------------------------------------------------------------

TEST(UploadEndpointTest, PublishesABuildEndToEnd) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = newBuild("End To End Title");
    const std::string content = "the game binary, such as it is";

    // 1. Negotiate: nothing has been uploaded yet, so the blob is missing.
    Json::Value blobs(Json::arrayValue);
    blobs.append(declaration(content));
    Json::Value negotiation;
    negotiation["blobs"] = blobs;

    const auto missing =
        harness().postJson("/api/v1/builds/" + publication.buildId + "/blobs/missing",
                           negotiation,
                           publication.token());
    ASSERT_EQ(missing->statusCode(), drogon::k200OK) << missing->body();
    ASSERT_EQ(bodyOf(missing)["missing"].size(), 1U);

    // 2. Transfer.
    const auto session = uploadBlob(publication, content);
    EXPECT_TRUE(session["complete"].asBool());
    EXPECT_EQ(session["status"].asString(), "completed");
    EXPECT_EQ(usedBytesFor(publication.userId()), static_cast<int64_t>(content.size()));

    // 3. Negotiating again now reports nothing missing — this is what makes the next build
    //    upload only what changed.
    const auto again =
        harness().postJson("/api/v1/builds/" + publication.buildId + "/blobs/missing",
                           negotiation,
                           publication.token());
    ASSERT_EQ(again->statusCode(), drogon::k200OK);
    EXPECT_EQ(bodyOf(again)["missing"].size(), 0U);

    // 4. Manifest.
    const auto finalized = harness().postJson("/api/v1/builds/" + publication.buildId + "/manifest",
                                              manifestPayload("Game.exe", content),
                                              publication.token());
    ASSERT_EQ(finalized->statusCode(), drogon::k200OK) << finalized->body();
    EXPECT_EQ(bodyOf(finalized)["status"].asString(), "ready");
    EXPECT_EQ(bodyOf(finalized)["fileCount"].asInt(), 1);
    EXPECT_EQ(bodyOf(finalized)["totalSizeBytes"].asInt64(), static_cast<int64_t>(content.size()));

    // 5. The manifest a client downloads hashes to what the build recorded.
    const auto manifest =
        harness().get("/api/v1/builds/" + publication.buildId + "/manifest", publication.token());
    ASSERT_EQ(manifest->statusCode(), drogon::k200OK);
    EXPECT_EQ(manifest->getHeader("X-Manifest-Sha256"),
              bodyOf(finalized)["manifestSha256"].asString());
    EXPECT_EQ(sha256Hex(manifest->body()), manifest->getHeader("X-Manifest-Sha256"))
        << "the served bytes must be exactly the bytes the hash covers";
}

TEST(UploadEndpointTest, TheFinishedBuildAppearsOnTheGameDetail) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = newBuild("Detailed Build Title");
    const std::string content = "shipped bytes";
    uploadBlob(publication, content);
    ASSERT_EQ(harness()
                  .postJson("/api/v1/builds/" + publication.buildId + "/manifest",
                            manifestPayload("Game.exe", content),
                            publication.token())
                  ->statusCode(),
              drogon::k200OK);

    const auto detail = harness().get("/api/v1/games/" + publication.gameId, publication.token());

    ASSERT_EQ(detail->statusCode(), drogon::k200OK);
    ASSERT_EQ(bodyOf(detail)["builds"].size(), 1U);
    EXPECT_EQ(bodyOf(detail)["builds"][0]["status"].asString(), "ready");
    EXPECT_EQ(bodyOf(detail)["builds"][0]["entrypoint"].asString(), "Game.exe");
}

// ---------------------------------------------------------------------------
// Resuming
// ---------------------------------------------------------------------------

TEST(UploadEndpointTest, ResumesAnInterruptedUploadFromTheRecordedOffset) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = newBuild("Resumed Upload Title");
    const std::string content = "a file long enough to arrive in two pieces";

    const auto started = beginUpload(publication, content);
    ASSERT_EQ(started->statusCode(), drogon::k201Created) << started->body();
    const auto uploadPath = "/api/v1/uploads/" + bodyOf(started)["id"].asString();

    const auto first =
        harness().patchBinary(uploadPath, content.substr(0, 12), 0, publication.token());
    ASSERT_EQ(first->statusCode(), drogon::k200OK) << first->body();
    EXPECT_EQ(first->getHeader("Upload-Offset"), "12");
    EXPECT_FALSE(bodyOf(first)["complete"].asBool());

    // The client comes back after losing its connection and asks where it got to.
    const auto status = harness().get(uploadPath, publication.token());
    ASSERT_EQ(status->statusCode(), drogon::k200OK);
    ASSERT_EQ(bodyOf(status)["receivedBytes"].asInt64(), 12);

    const auto second =
        harness().patchBinary(uploadPath, content.substr(12), 12, publication.token());

    ASSERT_EQ(second->statusCode(), drogon::k200OK) << second->body();
    EXPECT_TRUE(bodyOf(second)["complete"].asBool());
}

// A chunk written at the wrong place corrupts the file in a way only the final hash catches,
// after the whole upload has been paid for — so the wrong offset is refused up front.
TEST(UploadEndpointTest, RefusesAChunkAtTheWrongOffsetAndReportsTheRealOne) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = newBuild("Wrong Offset Title");
    const std::string content = "0123456789abcdef";

    const auto started = beginUpload(publication, content);
    ASSERT_EQ(started->statusCode(), drogon::k201Created);
    const auto uploadPath = "/api/v1/uploads/" + bodyOf(started)["id"].asString();
    ASSERT_EQ(harness()
                  .patchBinary(uploadPath, content.substr(0, 8), 0, publication.token())
                  ->statusCode(),
              drogon::k200OK);

    const auto replayed =
        harness().patchBinary(uploadPath, content.substr(0, 8), 0, publication.token());

    ASSERT_EQ(replayed->statusCode(), drogon::k409Conflict);
    EXPECT_NE(bodyOf(replayed)["detail"].asString().find("offset 8"), std::string::npos)
        << bodyOf(replayed)["detail"].asString();
}

TEST(UploadEndpointTest, RequiresAnExplicitUploadOffset) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = newBuild("Missing Offset Title");
    const std::string content = "some bytes";

    const auto started = beginUpload(publication, content);
    ASSERT_EQ(started->statusCode(), drogon::k201Created);

    const auto response = harness().patchBinary(
        "/api/v1/uploads/" + bodyOf(started)["id"].asString(), content, -1, publication.token());

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
    EXPECT_NE(bodyOf(response)["detail"].asString().find("Upload-Offset"), std::string::npos);
}

TEST(UploadEndpointTest, AbortingAnUploadEndsTheSession) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = newBuild("Aborted Upload Title");

    const auto started = beginUpload(publication, "abandoned content");
    ASSERT_EQ(started->statusCode(), drogon::k201Created);
    const auto uploadPath = "/api/v1/uploads/" + bodyOf(started)["id"].asString();

    ASSERT_EQ(harness().remove(uploadPath, publication.token())->statusCode(), drogon::k200OK);

    const auto afterwards =
        harness().patchBinary(uploadPath, "abandoned content", 0, publication.token());
    EXPECT_EQ(afterwards->statusCode(), drogon::k409Conflict);
}

// An interrupted upload stops being resumable once its window closes, or a client could park
// staging disk indefinitely.
TEST(UploadEndpointTest, RefusesToContinueAnExpiredUpload) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = newBuild("Expired Upload Title");
    const std::string content = "too slow";

    const auto started = beginUpload(publication, content);
    ASSERT_EQ(started->statusCode(), drogon::k201Created);
    const auto sessionId = bodyOf(started)["id"].asString();

    harness().database().exec("UPDATE upload_sessions SET expires_at = now() - interval '1 hour' "
                              "WHERE id = '" +
                              sessionId + "'::uuid");

    const auto response =
        harness().patchBinary("/api/v1/uploads/" + sessionId, content, 0, publication.token());

    EXPECT_EQ(response->statusCode(), drogon::k409Conflict);
}

// ---------------------------------------------------------------------------
// Integrity, ownership and quotas
// ---------------------------------------------------------------------------

TEST(UploadEndpointTest, DiscardsAnUploadWhoseBytesDoNotMatchTheDeclaredHash) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = newBuild("Corrupted Upload Title");
    const std::string promised = "what the publisher promised";
    const std::string sent = "what the publisher sent!!!!";
    ASSERT_EQ(promised.size(), sent.size()) << "the sizes must agree, only the bytes differ";

    const auto started = beginUpload(publication, promised);
    ASSERT_EQ(started->statusCode(), drogon::k201Created);

    const auto response = harness().patchBinary(
        "/api/v1/uploads/" + bodyOf(started)["id"].asString(), sent, 0, publication.token());

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
    EXPECT_EQ(harness().database().scalarInt("SELECT count(*) FROM blobs WHERE sha256 = '" +
                                             sha256Hex(promised) + "'"),
              0);
    EXPECT_EQ(usedBytesFor(publication.userId()), 0) << "a rejected upload must not consume quota";
}

TEST(UploadEndpointTest, RefusesUploadsToAnotherPublishersBuild) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = newBuild("Guarded Build Title");
    const auto intruder = harness().createSessionWithRole(uniqueEmail("intruder"), "dev");

    const auto response = harness().postJson("/api/v1/builds/" + publication.buildId + "/uploads",
                                             declaration("smuggled"),
                                             tokenOf(intruder));

    EXPECT_EQ(response->statusCode(), drogon::k404NotFound)
        << "an unowned build must not be distinguishable from a missing one";
}

TEST(UploadEndpointTest, RefusesUploadsFromAnAccountWithoutTheDevRole) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = newBuild("Player Blocked Title");
    const auto session = harness().createVerifiedSession(uniqueEmail("player"));

    const auto response = harness().postJson("/api/v1/builds/" + publication.buildId + "/uploads",
                                             declaration("not allowed"),
                                             tokenOf(session));

    EXPECT_EQ(response->statusCode(), drogon::k403Forbidden);
}

TEST(UploadEndpointTest, RefusesAnUploadThatWouldExceedTheQuota) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = newBuild("Over Quota Title");
    harness().database().exec("UPDATE users SET upload_quota_bytes = 8 WHERE id = '" +
                              publication.userId() + "'::uuid");

    const auto response = beginUpload(publication, "considerably more than eight bytes of content");

    EXPECT_EQ(response->statusCode(), drogon::k413RequestEntityTooLarge);
    EXPECT_EQ(bodyOf(response)["code"].asString(), "quota_exceeded");
}

// Content-addressed storage stores shared content once, so the second publisher to send it
// must not be charged for bytes that were already there.
TEST(UploadEndpointTest, DoesNotChargeTheQuotaForContentTheServerAlreadyHolds) {
    LAUNCHER_REQUIRE_DATABASE();
    const std::string shared = "an engine runtime shared between two games";

    const auto first = newBuild("Shared Content First Title");
    uploadBlob(first, shared);
    ASSERT_EQ(usedBytesFor(first.userId()), static_cast<int64_t>(shared.size()));

    const auto second = newBuild("Shared Content Second Title");
    const auto negotiated = harness().postJson(
        "/api/v1/builds/" + second.buildId + "/uploads", declaration(shared), second.token());

    EXPECT_EQ(negotiated->statusCode(), drogon::k409Conflict)
        << "the server already holds this content and says so";
    EXPECT_EQ(usedBytesFor(second.userId()), 0);
}

// ---------------------------------------------------------------------------
// Manifests
// ---------------------------------------------------------------------------

TEST(UploadEndpointTest, RefusesToFinalizeWhileBlobsAreStillMissing) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = newBuild("Incomplete Manifest Title");

    const auto response = harness().postJson("/api/v1/builds/" + publication.buildId + "/manifest",
                                             manifestPayload("Game.exe", "never uploaded"),
                                             publication.token());

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
    EXPECT_NE(bodyOf(response)["detail"].asString().find(sha256Hex("never uploaded")),
              std::string::npos);
}

// A path that escapes the install directory must be unstorable, both here and — as defence in
// depth — at the CHECK constraint on build_files.
TEST(UploadEndpointTest, RefusesAManifestWithATraversingPath) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = newBuild("Traversal Manifest Title");
    const std::string content = "payload";
    uploadBlob(publication, content);

    const auto response = harness().postJson("/api/v1/builds/" + publication.buildId + "/manifest",
                                             manifestPayload("../escape.exe", content),
                                             publication.token());

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
}

TEST(UploadEndpointTest, RefusesAnEntrypointThatIsNotInTheManifest) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = newBuild("Missing Entrypoint Title");
    const std::string content = "payload";
    uploadBlob(publication, content);

    auto payload = manifestPayload("Game.exe", content);
    payload["entrypoint"] = "Launcher.exe";
    const auto response = harness().postJson(
        "/api/v1/builds/" + publication.buildId + "/manifest", payload, publication.token());

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
}

TEST(UploadEndpointTest, RefusesToFinalizeTheSameBuildTwice) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = newBuild("Twice Finalized Title");
    const std::string content = "payload";
    uploadBlob(publication, content);

    const auto payload = manifestPayload("Game.exe", content);
    ASSERT_EQ(harness()
                  .postJson("/api/v1/builds/" + publication.buildId + "/manifest",
                            payload,
                            publication.token())
                  ->statusCode(),
              drogon::k200OK);

    const auto again = harness().postJson(
        "/api/v1/builds/" + publication.buildId + "/manifest", payload, publication.token());

    EXPECT_EQ(again->statusCode(), drogon::k409Conflict);
}

TEST(UploadEndpointTest, HidesTheManifestOfABuildThatIsNotReady) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publication = newBuild("Unfinished Manifest Title");

    const auto response =
        harness().get("/api/v1/builds/" + publication.buildId + "/manifest", publication.token());

    EXPECT_EQ(response->statusCode(), drogon::k404NotFound);
}

} // namespace
