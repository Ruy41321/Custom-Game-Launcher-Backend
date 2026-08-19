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

std::string idOf(const Json::Value& session) {
    return session["user"]["id"].asString();
}

/// An operator, plus a second one so the last-administrator guard is not in the way of tests
/// that are about something else. Both are real accounts with real roles.
Json::Value anOperator(const std::string& prefix = "adminuser") {
    return harness().createSessionWithRole(uniqueEmail(prefix), "admin");
}

Json::Value aPlayer(const std::string& prefix = "subject") {
    return harness().createVerifiedSession(uniqueEmail(prefix));
}

int auditRowsFor(const std::string& userId, const std::string& action) {
    return harness().database().scalarInt("SELECT count(*) FROM audit_log WHERE entity_id = '" +
                                          userId + "' AND action = '" + action + "'");
}

} // namespace

// ---------------------------------------------------------------------------
// The gate again, on the surface that actually matters
// ---------------------------------------------------------------------------

TEST(AdminUserEndpointTest, UserManagementDoesNotExistOnThePublicListener) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();

    // The single most damaging route to leak, tested against the listener it must never
    // answer on — with a token that would otherwise be accepted.
    const auto response = harness().get("/api/v1/../admin/api/users", tokenOf(session));
    EXPECT_EQ(response->statusCode(), drogon::k404NotFound) << response->body();

    EXPECT_EQ(harness().get("/admin/api/users", tokenOf(session))->statusCode(),
              drogon::k404NotFound);
}

TEST(AdminUserEndpointTest, RefusesAPublisherToken) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = harness().createSessionWithRole(uniqueEmail("adminuser-dev"), "dev");

    EXPECT_EQ(harness().adminGet("/admin/api/users", tokenOf(session))->statusCode(),
              drogon::k403Forbidden);
}

// ---------------------------------------------------------------------------
// Listing and reading
// ---------------------------------------------------------------------------

TEST(AdminUserEndpointTest, ListsAccountsAndFindsOneBySearch) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();
    const auto subject = aPlayer();
    const auto subjectEmail = subject["user"]["email"].asString();

    const auto response =
        harness().adminGet("/admin/api/users?search=" + subjectEmail, tokenOf(session));

    ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
    const auto body = bodyOf(response);
    ASSERT_EQ(body["items"].size(), 1U) << response->body();
    EXPECT_EQ(body["items"][0]["email"].asString(), subjectEmail);
    EXPECT_TRUE(body["items"][0]["active"].asBool());
}

TEST(AdminUserEndpointTest, AnAccountNeverCarriesItsPasswordHash) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();
    const auto subject = aPlayer();

    const auto body =
        bodyOf(harness().adminGet("/admin/api/users/" + idOf(subject), tokenOf(session)));

    // The column is not even selected, so this asserts a property of the query rather than of
    // the serialiser. Either would do; both together mean a future field added to the response
    // cannot leak it by accident.
    EXPECT_FALSE(body.isMember("passwordHash"));
    EXPECT_FALSE(body.isMember("password_hash"));
}

TEST(AdminUserEndpointTest, ReportsAMistypedIdAsMissingRatherThanFailing) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();

    // Without the guard this reaches a `$n::uuid` comparison and PostgreSQL raises, which the
    // caller would see as a 500 for what is plainly a typo.
    EXPECT_EQ(harness().adminGet("/admin/api/users/not-a-uuid", tokenOf(session))->statusCode(),
              drogon::k404NotFound);
}

// ---------------------------------------------------------------------------
// Quotas, and the audit row that must accompany them
// ---------------------------------------------------------------------------

TEST(AdminUserEndpointTest, ChangesAQuotaAndLeavesExactlyOneAuditRow) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();
    const auto subject = aPlayer();

    Json::Value body;
    body["uploadQuotaBytes"] = 123456789;
    const auto response =
        harness().adminPatchJson("/admin/api/users/" + idOf(subject), body, tokenOf(session));

    ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
    EXPECT_EQ(bodyOf(response)["uploadQuotaBytes"].asInt64(), 123456789);

    // The row and the change are one statement, so this is not a check that a second write
    // happened to succeed — it is a check that the statement is shaped the way it claims.
    EXPECT_EQ(auditRowsFor(idOf(subject), "user.quota.changed"), 1);
}

TEST(AdminUserEndpointTest, ARefusedChangeRecordsNothing) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();
    const auto subject = aPlayer();

    Json::Value body;
    body["uploadQuotaBytes"] = -1;
    EXPECT_EQ(harness()
                  .adminPatchJson("/admin/api/users/" + idOf(subject), body, tokenOf(session))
                  ->statusCode(),
              drogon::k422UnprocessableEntity);

    EXPECT_EQ(auditRowsFor(idOf(subject), "user.quota.changed"), 0);
}

TEST(AdminUserEndpointTest, AChangeToAnAccountThatDoesNotExistRecordsNothing) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();
    const std::string absent = "00000000-0000-0000-0000-0000000000ff";

    Json::Value body;
    body["uploadQuotaBytes"] = 42;
    EXPECT_EQ(harness()
                  .adminPatchJson("/admin/api/users/" + absent, body, tokenOf(session))
                  ->statusCode(),
              drogon::k404NotFound);

    // The audit arm selects from the updating arm, so an UPDATE that matched nothing writes
    // nothing. This is the case a separate insert afterwards would get wrong.
    EXPECT_EQ(auditRowsFor(absent, "user.quota.changed"), 0);
}

TEST(AdminUserEndpointTest, AnEmptyPatchChangesNothingAndRecordsNothing) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();
    const auto subject = aPlayer();

    const auto response = harness().adminPatchJson(
        "/admin/api/users/" + idOf(subject), Json::Value(Json::objectValue), tokenOf(session));

    ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
    EXPECT_EQ(auditRowsFor(idOf(subject), "user.quota.changed"), 0);
    EXPECT_EQ(auditRowsFor(idOf(subject), "user.deactivated"), 0);
}

// ---------------------------------------------------------------------------
// Deactivation
// ---------------------------------------------------------------------------

TEST(AdminUserEndpointTest, DeactivatingAnAccountEndsItsSessions) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();
    const auto subject = aPlayer();

    Json::Value refresh;
    refresh["refreshToken"] = subject["refreshToken"];

    Json::Value body;
    body["active"] = false;
    ASSERT_EQ(harness()
                  .adminPatchJson("/admin/api/users/" + idOf(subject), body, tokenOf(session))
                  ->statusCode(),
              drogon::k200OK);

    // Revoked in the same statement as the flag. Leaving the tokens alive would mean a
    // disabled account keeps renewing access for as long as somebody holds one, which is not
    // what disabling an account means to anybody.
    EXPECT_EQ(harness().postJson("/api/v1/auth/refresh", refresh)->statusCode(),
              drogon::k401Unauthorized);
    EXPECT_EQ(auditRowsFor(idOf(subject), "user.deactivated"), 1);
}

TEST(AdminUserEndpointTest, RefusesToDeactivateTheOperatorsOwnAccount) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();

    Json::Value body;
    body["active"] = false;
    const auto response =
        harness().adminPatchJson("/admin/api/users/" + idOf(session), body, tokenOf(session));

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity) << response->body();
}

// ---------------------------------------------------------------------------
// Roles
// ---------------------------------------------------------------------------

TEST(AdminUserEndpointTest, GrantsAndRevokesTheDevlist) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();
    const auto subject = aPlayer();
    const auto path = "/admin/api/users/" + idOf(subject) + "/roles/dev";

    // This is the manual INSERT the setup notes have carried since milestone 4, finally as an
    // endpoint.
    const auto granted = harness().adminPut(path, tokenOf(session));
    ASSERT_EQ(granted->statusCode(), drogon::k200OK) << granted->body();

    const auto roles = bodyOf(granted)["roles"];
    bool holdsDev = false;
    for (const auto& role : roles) {
        holdsDev = holdsDev || role.asString() == "dev";
    }
    EXPECT_TRUE(holdsDev) << granted->body();
    EXPECT_EQ(auditRowsFor(idOf(subject), "user.role.granted"), 1);

    ASSERT_EQ(harness().adminRemove(path, tokenOf(session))->statusCode(), drogon::k200OK);
    EXPECT_EQ(auditRowsFor(idOf(subject), "user.role.revoked"), 1);
}

TEST(AdminUserEndpointTest, GrantingTheSameRoleTwiceRecordsItOnce) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();
    const auto subject = aPlayer();
    const auto path = "/admin/api/users/" + idOf(subject) + "/roles/dev";

    EXPECT_EQ(harness().adminPut(path, tokenOf(session))->statusCode(), drogon::k200OK);
    EXPECT_EQ(harness().adminPut(path, tokenOf(session))->statusCode(), drogon::k200OK);

    // ON CONFLICT DO NOTHING returns no row, and the audit arm selects from that row, so the
    // second grant records nothing without any code deciding it should not.
    EXPECT_EQ(auditRowsFor(idOf(subject), "user.role.granted"), 1);
}

TEST(AdminUserEndpointTest, RejectsARoleThatDoesNotExist) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();
    const auto subject = aPlayer();

    const auto response =
        harness().adminPut("/admin/api/users/" + idOf(subject) + "/roles/wizard", tokenOf(session));

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity) << response->body();
}

TEST(AdminUserEndpointTest, ListsTheRolesTheSchemaSeeded) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();

    const auto body = bodyOf(harness().adminGet("/admin/api/roles", tokenOf(session)));

    ASSERT_GE(body["items"].size(), 3U);
    bool sawAdmin = false;
    for (const auto& role : body["items"]) {
        sawAdmin = sawAdmin || role["key"].asString() == "admin";
    }
    EXPECT_TRUE(sawAdmin);
}

// ---------------------------------------------------------------------------
// The trail itself
// ---------------------------------------------------------------------------

TEST(AdminAuditEndpointTest, ReportsWhoChangedWhatAndToWhichValue) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();
    const auto subject = aPlayer();

    Json::Value change;
    change["uploadQuotaBytes"] = 777;
    ASSERT_EQ(harness()
                  .adminPatchJson("/admin/api/users/" + idOf(subject), change, tokenOf(session))
                  ->statusCode(),
              drogon::k200OK);

    const auto body = bodyOf(harness().adminGet("/admin/api/audit?entityId=" + idOf(subject) +
                                                    "&action=user.quota.changed",
                                                tokenOf(session)));

    ASSERT_EQ(body["items"].size(), 1U) << body.toStyledString();
    const auto entry = body["items"][0];
    EXPECT_EQ(entry["actorUserId"].asString(), idOf(session));
    EXPECT_EQ(entry["actorEmail"].asString(), session["user"]["email"].asString());
    EXPECT_EQ(entry["entityType"].asString(), "user");
    // metadata is a real object, not a string of JSON: it is the one field whose shape varies
    // by action, and quoting it would push the parsing onto the page.
    EXPECT_TRUE(entry["metadata"].isObject());
    EXPECT_EQ(entry["metadata"]["quotaBytes"].asString(), "777");
}

// ---------------------------------------------------------------------------
// One-time passwords — the way back in where the deployment sends no mail
// ---------------------------------------------------------------------------

TEST(AdminUserEndpointTest, HandsOutAPasswordThatSignsInAndDemandsToBeReplaced) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator("tempuser");
    const auto subject = aPlayer("tempsubject");

    const auto issued =
        harness().adminPostJson("/admin/api/users/" + idOf(subject) + "/temporary-password",
                                Json::Value{},
                                tokenOf(session));
    ASSERT_EQ(issued->statusCode(), drogon::k200OK) << issued->body();

    const auto body = bodyOf(issued);
    const auto temporary = body["temporaryPassword"].asString();
    ASSERT_FALSE(temporary.empty()) << body.toStyledString();
    EXPECT_TRUE(body["user"]["passwordChangeRequired"].asBool());

    // It really is the account's password now, and the session it produces says so out loud —
    // the client is told rather than left to discover it from the first refusal.
    Json::Value credentials;
    credentials["email"] = subject["user"]["email"].asString();
    credentials["password"] = temporary;

    const auto signedIn = harness().postJson("/api/v1/auth/login", credentials);
    ASSERT_EQ(signedIn->statusCode(), drogon::k200OK) << signedIn->body();
    EXPECT_TRUE(bodyOf(signedIn)["user"]["passwordChangeRequired"].asBool());

    EXPECT_EQ(auditRowsFor(idOf(subject), "user.password.temporary_set"), 1);
}

// Every session the old password reached dies with it, or handing out a temporary password
// would leave whoever was already signed in exactly where they were.
TEST(AdminUserEndpointTest, TheAccountsExistingSessionsStopWorking) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator("tempkill");
    const auto subject = aPlayer("tempkillsubject");

    ASSERT_EQ(harness().get("/api/v1/library", tokenOf(subject))->statusCode(), drogon::k200OK);

    ASSERT_EQ(harness()
                  .adminPostJson("/admin/api/users/" + idOf(subject) + "/temporary-password",
                                 Json::Value{},
                                 tokenOf(session))
                  ->statusCode(),
              drogon::k200OK);

    Json::Value refresh;
    refresh["refreshToken"] = subject["refreshToken"].asString();
    EXPECT_EQ(harness().postJson("/api/v1/auth/refresh", refresh)->statusCode(),
              drogon::k401Unauthorized);
}

TEST(AdminUserEndpointTest, RefusesATemporaryPasswordToANonOperator) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto publisher = harness().createSessionWithRole(uniqueEmail("tempdev"), "dev");
    const auto subject = aPlayer("tempvictim");

    EXPECT_EQ(harness()
                  .adminPostJson("/admin/api/users/" + idOf(subject) + "/temporary-password",
                                 Json::Value{},
                                 tokenOf(publisher))
                  ->statusCode(),
              drogon::k403Forbidden);

    EXPECT_EQ(auditRowsFor(idOf(subject), "user.password.temporary_set"), 0);
}

// The route that hands out a credential must not answer on the listener the internet reaches.
TEST(AdminUserEndpointTest, TheTemporaryPasswordRouteDoesNotExistOnThePublicListener) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator("temppublic");
    const auto subject = aPlayer("temppublicsubject");

    EXPECT_EQ(harness()
                  .postJson("/admin/api/users/" + idOf(subject) + "/temporary-password",
                            Json::Value{},
                            tokenOf(session))
                  ->statusCode(),
              drogon::k404NotFound);
}

TEST(AdminUserEndpointTest, AnOperatorCannotSetATemporaryPasswordOnThemselves) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator("tempself");

    const auto refused =
        harness().adminPostJson("/admin/api/users/" + idOf(session) + "/temporary-password",
                                Json::Value{},
                                tokenOf(session));

    EXPECT_EQ(refused->statusCode(), drogon::k422UnprocessableEntity) << refused->body();
    EXPECT_EQ(auditRowsFor(idOf(session), "user.password.temporary_set"), 0);
}

TEST(AdminUserEndpointTest, AnAccountThatIsNotThereGetsNoPassword) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator("tempmissing");

    EXPECT_EQ(harness()
                  .adminPostJson("/admin/api/users/00000000-0000-0000-0000-000000000000/"
                                 "temporary-password",
                                 Json::Value{},
                                 tokenOf(session))
                  ->statusCode(),
              drogon::k404NotFound);

    EXPECT_EQ(harness()
                  .adminPostJson("/admin/api/users/not-a-uuid/temporary-password",
                                 Json::Value{},
                                 tokenOf(session))
                  ->statusCode(),
              drogon::k404NotFound);
}

TEST(AdminAuditEndpointTest, FiltersByActor) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto first = anOperator("audit-first");
    const auto second = anOperator("audit-second");
    const auto subject = aPlayer();

    Json::Value change;
    change["uploadQuotaBytes"] = 10;
    ASSERT_EQ(harness()
                  .adminPatchJson("/admin/api/users/" + idOf(subject), change, tokenOf(first))
                  ->statusCode(),
              drogon::k200OK);

    const auto body = bodyOf(harness().adminGet(
        "/admin/api/audit?actor=" + idOf(second) + "&entityId=" + idOf(subject), tokenOf(first)));

    EXPECT_EQ(body["items"].size(), 0U) << body.toStyledString();
}

TEST(AdminAuditEndpointTest, RefusesAnActorFilterThatIsNotAUserId) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();

    // Refused rather than ignored: silently dropping the filter would answer a question
    // nobody asked, and a whole page of results would look like the answer.
    const auto response = harness().adminGet("/admin/api/audit?actor=nonsense", tokenOf(session));

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity) << response->body();
}
