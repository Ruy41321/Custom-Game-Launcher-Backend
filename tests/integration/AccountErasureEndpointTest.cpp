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
    return prefix + std::to_string(counter.fetch_add(1)) + "@example.test";
}

/// Returns a value: iterating into the temporary `bodyOf` would otherwise return walks freed
/// memory, which is the trap recorded in CLAUDE.md section 8.
Json::Value bodyOf(const drogon::HttpResponsePtr& response) {
    EXPECT_NE(response, nullptr);
    const auto json = response->getJsonObject();
    EXPECT_NE(json, nullptr) << "expected a JSON body";
    return json == nullptr ? Json::Value{} : *json;
}

std::string tokenOf(const Json::Value& session) {
    return session["accessToken"].asString();
}

std::string idOf(const Json::Value& session) {
    return session["user"]["id"].asString();
}

/// The password `AppHarness::createVerifiedSession` registers every account with.
constexpr const char* SESSION_PASSWORD = "correct horse battery staple";

drogon::HttpResponsePtr eraseWith(const Json::Value& session,
                                  const std::string& password = SESSION_PASSWORD,
                                  const std::string& reason = {}) {
    Json::Value body;
    body["password"] = password;
    if (!reason.empty()) {
        body["reason"] = reason;
    }
    return harness().postJson("/api/v1/me/deletion", body, tokenOf(session));
}

/// A publisher with one public game, a ready build, that game in its own library, and one
/// download event to its name — everything an erasure has to have an answer for.
struct Departing {
    Json::Value session;
    std::string userId;
    std::string gameId;
    std::string buildId;

    std::string token() const { return tokenOf(session); }
};

Departing departingPublisher(const std::string& title, const std::string& content) {
    Departing departing;
    departing.session = harness().createSessionWithRole(uniqueEmail("erasure"), "dev");
    departing.userId = idOf(departing.session);
    const auto token = departing.token();

    Json::Value game;
    game["title"] = title;
    game["visibility"] = "public";
    departing.gameId = bodyOf(harness().postJson("/api/v1/games", game, token))["id"].asString();

    Json::Value version;
    version["semver"] = "1.0.0";
    version["publish"] = true;
    const auto versionId = bodyOf(harness().postJson(
        "/api/v1/games/" + departing.gameId + "/versions", version, token))["id"]
                               .asString();

    Json::Value build;
    build["platform"] = "windows";
    departing.buildId = bodyOf(harness().postJson("/api/v1/games/" + departing.gameId +
                                                      "/versions/" + versionId + "/builds",
                                                  build,
                                                  token))["id"]
                            .asString();

    Json::Value declaration;
    declaration["sha256"] = sha256Hex(content);
    declaration["size"] = static_cast<Json::Int64>(content.size());
    const auto negotiated =
        harness().postJson("/api/v1/builds/" + departing.buildId + "/uploads", declaration, token);
    // A 409 is the deduplication working: another test already stored these exact bytes.
    if (negotiated->statusCode() != drogon::k409Conflict) {
        EXPECT_EQ(negotiated->statusCode(), drogon::k201Created) << negotiated->body();
        const auto uploadId = bodyOf(negotiated)["id"].asString();
        EXPECT_EQ(
            harness().patchBinary("/api/v1/uploads/" + uploadId, content, 0, token)->statusCode(),
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
                  .postJson("/api/v1/builds/" + departing.buildId + "/manifest", manifest, token)
                  ->statusCode(),
              drogon::k200OK);

    EXPECT_EQ(harness().put("/api/v1/library/" + departing.gameId, token)->statusCode(),
              drogon::k200OK);
    // No body at all, which is how a first install asks for a plan.
    EXPECT_EQ(
        harness().post("/api/v1/builds/" + departing.buildId + "/download", token)->statusCode(),
        drogon::k200OK);

    return departing;
}

int countWhere(const std::string& sql) {
    return harness().database().scalarInt(sql);
}

// ---------------------------------------------------------------------------
// What an erasure leaves behind
// ---------------------------------------------------------------------------

TEST(AccountErasureEndpointTest, AnonymisesTheAccountAndEmptiesEverythingPersonal) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto departing = departingPublisher("Erasure Subject", "erasure content");
    const auto email = departing.session["user"]["email"].asString();

    const auto response = eraseWith(departing.session, SESSION_PASSWORD, "moving on");

    ASSERT_EQ(response->statusCode(), drogon::k204NoContent) << response->body();
    EXPECT_EQ(countWhere("SELECT count(*) FROM users WHERE email = '" + email + "'"), 0)
        << "the address the person used must not still be on the row";
    EXPECT_EQ(countWhere("SELECT count(*) FROM users WHERE id = '" + departing.userId +
                         "' AND display_name = 'Deleted account' AND NOT is_active"
                         " AND email_verified_at IS NULL"),
              1)
        << "the row survives, anonymous and disabled";

    EXPECT_EQ(countWhere("SELECT count(*) FROM refresh_tokens WHERE user_id = '" +
                         departing.userId + "'"),
              0)
        << "a live session after an erasure is the one thing an erasure exists to rule out";
    EXPECT_EQ(
        countWhere("SELECT count(*) FROM user_tokens WHERE user_id = '" + departing.userId + "'"),
        0);
    EXPECT_EQ(
        countWhere("SELECT count(*) FROM user_games WHERE user_id = '" + departing.userId + "'"),
        0);
}

TEST(AccountErasureEndpointTest, LeavesThePublishedGamesAndTheirBuildsStanding) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto departing = departingPublisher("Outliving Publisher", "outliving content");
    const auto reader = harness().createVerifiedSession(uniqueEmail("erasurereader"));

    ASSERT_EQ(eraseWith(departing.session)->statusCode(), drogon::k204NoContent);

    // Somebody else's installed copy still has to be able to update, which is precisely why the
    // erasure anonymises the publisher instead of deleting it: publisher_user_id is RESTRICT.
    const auto detail = bodyOf(harness().get("/api/v1/games/" + departing.gameId, tokenOf(reader)));
    EXPECT_EQ(detail["game"]["id"].asString(), departing.gameId);
    EXPECT_EQ(detail["game"]["publisher"]["displayName"].asString(), "Deleted account");
    EXPECT_EQ(harness()
                  .get("/api/v1/builds/" + departing.buildId + "/manifest", tokenOf(reader))
                  ->statusCode(),
              drogon::k200OK);
}

TEST(AccountErasureEndpointTest, KeepsTheDownloadEventAndDropsWhoCausedIt) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto departing = departingPublisher("Counted Download", "counted content");
    ASSERT_GE(countWhere("SELECT count(*) FROM download_events WHERE user_id = '" +
                         departing.userId + "'"),
              1);

    ASSERT_EQ(eraseWith(departing.session)->statusCode(), drogon::k204NoContent);

    EXPECT_EQ(countWhere("SELECT count(*) FROM download_events WHERE user_id = '" +
                         departing.userId + "'"),
              0);
    // The row itself stays: `download_events.user_id` was made nullable in the first migration
    // for exactly this, so the deployment's totals do not change when somebody leaves.
    EXPECT_GE(countWhere("SELECT count(*) FROM download_events WHERE game_id = '" +
                         departing.gameId + "' AND user_id IS NULL"),
              1);
}

TEST(AccountErasureEndpointTest, RecordsTheErasureInTheAuditTrailAndTheRequestTable) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto departing = departingPublisher("Audited Erasure", "audited content");

    ASSERT_EQ(eraseWith(departing.session, SESSION_PASSWORD, "no longer needed")->statusCode(),
              drogon::k204NoContent);

    EXPECT_EQ(countWhere("SELECT count(*) FROM audit_log WHERE action = 'user.erased'"
                         " AND entity_id = '" +
                         departing.userId + "' AND actor_user_id = '" + departing.userId + "'"),
              1);
    EXPECT_EQ(countWhere("SELECT count(*) FROM account_deletion_requests WHERE user_id = '" +
                         departing.userId +
                         "' AND status = 'completed'"
                         " AND reason = 'no longer needed' AND processed_at IS NOT NULL"),
              1);
}

TEST(AccountErasureEndpointTest, TwoErasedAccountsDoNotCollideOnTheReplacementAddress) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto first = harness().createVerifiedSession(uniqueEmail("erasurepair"));
    const auto second = harness().createVerifiedSession(uniqueEmail("erasurepair"));

    ASSERT_EQ(eraseWith(first)->statusCode(), drogon::k204NoContent);
    // users.email is citext UNIQUE, so a shared placeholder would make this one a 500.
    ASSERT_EQ(eraseWith(second)->statusCode(), drogon::k204NoContent);

    EXPECT_EQ(countWhere("SELECT count(DISTINCT email) FROM users WHERE id IN ('" + idOf(first) +
                         "', '" + idOf(second) + "')"),
              2);
}

// ---------------------------------------------------------------------------
// What an erased account can no longer do
// ---------------------------------------------------------------------------

TEST(AccountErasureEndpointTest, AnErasedAccountCanNeitherSignInNorRefresh) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail("erasedlogin");
    const auto session = harness().createVerifiedSession(email);
    const auto refreshToken = session["refreshToken"].asString();
    ASSERT_EQ(eraseWith(session)->statusCode(), drogon::k204NoContent);

    Json::Value credentials;
    credentials["email"] = email;
    credentials["password"] = SESSION_PASSWORD;
    EXPECT_EQ(harness().postJson("/api/v1/auth/login", credentials)->statusCode(),
              drogon::k401Unauthorized);

    Json::Value rotation;
    rotation["refreshToken"] = refreshToken;
    EXPECT_EQ(harness().postJson("/api/v1/auth/refresh", rotation)->statusCode(),
              drogon::k401Unauthorized);
}

TEST(AccountErasureEndpointTest, ASecondErasureOnAStillValidTokenChangesNothing) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = harness().createVerifiedSession(uniqueEmail("erasedtwice"));
    ASSERT_EQ(eraseWith(session)->statusCode(), drogon::k204NoContent);

    // The access token stays cryptographically valid for its remaining minutes; the password
    // behind it does not exist any more, and that is what stops the second attempt.
    EXPECT_EQ(eraseWith(session)->statusCode(), drogon::k401Unauthorized);
    EXPECT_EQ(countWhere("SELECT count(*) FROM audit_log WHERE action = 'user.erased'"
                         " AND entity_id = '" +
                         idOf(session) + "'"),
              1);
}

TEST(AccountErasureEndpointTest, RefusesWithoutTheCurrentPassword) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = harness().createVerifiedSession(uniqueEmail("erasurewrongpw"));

    const auto response = eraseWith(session, "not the password");

    EXPECT_EQ(response->statusCode(), drogon::k401Unauthorized) << response->body();
    EXPECT_EQ(
        countWhere("SELECT count(*) FROM users WHERE id = '" + idOf(session) + "' AND is_active"),
        1);
}

TEST(AccountErasureEndpointTest, RefusesWithoutAToken) {
    LAUNCHER_REQUIRE_DATABASE();

    Json::Value body;
    body["password"] = SESSION_PASSWORD;
    EXPECT_EQ(harness().postJson("/api/v1/me/deletion", body)->statusCode(),
              drogon::k401Unauthorized);
}

TEST(AccountErasureEndpointTest, RefusesABodyWithNoPassword) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = harness().createVerifiedSession(uniqueEmail("erasurenopw"));

    EXPECT_EQ(harness()
                  .postJson("/api/v1/me/deletion", Json::Value(Json::objectValue), tokenOf(session))
                  ->statusCode(),
              drogon::k422UnprocessableEntity);
}

} // namespace
