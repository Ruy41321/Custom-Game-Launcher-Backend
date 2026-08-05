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

Json::Value report(const std::string& exceptionType = "System.IO.IOException",
                   const std::string& stack = "   at Fetcher.FetchAsync() in F.cs:line 42") {
    Json::Value json;
    json["kind"] = "unhandled";
    json["occurredAt"] = "2026-08-05T12:00:00Z";
    json["launcherVersion"] = "0.1.0";
    json["platform"] = "Microsoft Windows NT 10.0.26200.0";
    json["exceptionType"] = exceptionType;
    json["message"] = "something broke";
    json["stackTrace"] = stack;
    return json;
}

/// A distinct stack per test, so tests that count occurrences are not affected by every other
/// test in the binary — the integration binary shares one database.
std::string uniqueStack() {
    static std::atomic<int> counter{0};
    return "   at Test" + std::to_string(counter.fetch_add(1)) + ".Method() in T.cs:line 1";
}

int countWhere(const std::string& sql) {
    return harness().database().scalarInt(sql);
}

// ---------------------------------------------------------------------------
// Submitting: no token, and none wanted
// ---------------------------------------------------------------------------

TEST(CrashReportEndpointTest, AcceptsAReportFromAClientThatHasNeverSignedIn) {
    LAUNCHER_REQUIRE_DATABASE();

    const auto response = harness().postJson("/api/v1/crash-reports", report());

    // 202 rather than 201: the client is told it was taken, and there is no resource it can go
    // and read afterwards.
    ASSERT_EQ(response->statusCode(), drogon::k202Accepted) << response->body();
    EXPECT_EQ(bodyOf(response)["fingerprint"].asString().size(), 64U);
}

/// The decision at the head of migration 0004, asserted rather than left to a comment: a crash
/// report names no account, so there is no column for one to end up in.
TEST(CrashReportEndpointTest, StoresNothingThatNamesAnAccount) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = harness().createVerifiedSession(uniqueEmail("crashuser"));

    // Sent *with* a valid token, which the route neither needs nor looks at.
    ASSERT_EQ(harness().postJson("/api/v1/crash-reports", report(), tokenOf(session))->statusCode(),
              drogon::k202Accepted);

    EXPECT_EQ(countWhere("SELECT count(*) FROM information_schema.columns"
                         " WHERE table_name = 'crash_reports' AND column_name = 'user_id'"),
              0);
}

TEST(CrashReportEndpointTest, RefusesAReportWithNoKind) {
    LAUNCHER_REQUIRE_DATABASE();

    auto malformed = report();
    malformed.removeMember("kind");

    EXPECT_EQ(harness().postJson("/api/v1/crash-reports", malformed)->statusCode(),
              drogon::k422UnprocessableEntity);
}

TEST(CrashReportEndpointTest, RefusesATimestampThatIsNotOne) {
    LAUNCHER_REQUIRE_DATABASE();

    auto malformed = report();
    malformed["occurredAt"] = "yesterday";

    // A 422 and not a 500: the guard exists so a bad literal never reaches a timestamptz cast.
    EXPECT_EQ(harness().postJson("/api/v1/crash-reports", malformed)->statusCode(),
              drogon::k422UnprocessableEntity);
}

TEST(CrashReportEndpointTest, RefusesAStackTraceTooLargeToStore) {
    LAUNCHER_REQUIRE_DATABASE();

    auto oversized = report();
    oversized["stackTrace"] = std::string(64 * 1024, 'x');

    EXPECT_EQ(harness().postJson("/api/v1/crash-reports", oversized)->statusCode(),
              drogon::k422UnprocessableEntity);
}

TEST(CrashReportEndpointTest, TheSameCrashTwiceCarriesTheSameFingerprint) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto stack = uniqueStack();

    const auto first =
        bodyOf(harness().postJson("/api/v1/crash-reports", report("System.IO.IOException", stack)));
    const auto second =
        bodyOf(harness().postJson("/api/v1/crash-reports", report("System.IO.IOException", stack)));

    EXPECT_EQ(first["fingerprint"].asString(), second["fingerprint"].asString());
}

// ---------------------------------------------------------------------------
// Reading: the operator surface, and only there
// ---------------------------------------------------------------------------

TEST(CrashReportEndpointTest, CrashesDoNotExistOnThePublicListener) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = harness().createSessionWithRole(uniqueEmail("crashadmin"), "admin");

    EXPECT_EQ(harness().get("/admin/api/crashes", tokenOf(session))->statusCode(),
              drogon::k404NotFound);
}

TEST(CrashReportEndpointTest, APublisherCannotReadThem) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = harness().createSessionWithRole(uniqueEmail("crashdev"), "dev");

    EXPECT_EQ(harness().adminGet("/admin/api/crashes", tokenOf(session))->statusCode(),
              drogon::k403Forbidden);
}

TEST(CrashReportEndpointTest, AnOperatorSeesOneRowPerBugRatherThanOnePerReport) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = harness().createSessionWithRole(uniqueEmail("crashadmin"), "admin");
    const auto stack = uniqueStack();

    for (int index = 0; index < 3; ++index) {
        ASSERT_EQ(harness()
                      .postJson("/api/v1/crash-reports", report("System.IO.IOException", stack))
                      ->statusCode(),
                  drogon::k202Accepted);
    }

    const auto fingerprint = bodyOf(harness().postJson(
        "/api/v1/crash-reports", report("System.IO.IOException", stack)))["fingerprint"]
                                 .asString();

    const auto groups =
        bodyOf(harness().adminGet("/admin/api/crashes?pageSize=100", tokenOf(session)));

    bool found = false;
    for (const auto& group : groups["items"]) {
        if (group["fingerprint"].asString() == fingerprint) {
            found = true;
            EXPECT_EQ(group["occurrences"].asInt64(), 4);
            EXPECT_FALSE(group["latestReportId"].asString().empty());
        }
    }
    EXPECT_TRUE(found) << "the bug just reported should be in the list";
}

TEST(CrashReportEndpointTest, TheReportsBehindOneBugCanBeOpened) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = harness().createSessionWithRole(uniqueEmail("crashadmin"), "admin");
    const auto stack = uniqueStack();

    const auto fingerprint = bodyOf(harness().postJson(
        "/api/v1/crash-reports", report("System.IO.IOException", stack)))["fingerprint"]
                                 .asString();

    const auto page = bodyOf(harness().adminGet(
        "/admin/api/crashes/reports?fingerprint=" + fingerprint, tokenOf(session)));

    ASSERT_EQ(page["items"].size(), 1U);
    const auto reportId = page["items"][0]["id"].asString();
    EXPECT_EQ(page["items"][0]["stackTrace"].asString(), stack);

    const auto one =
        bodyOf(harness().adminGet("/admin/api/crashes/reports/" + reportId, tokenOf(session)));
    EXPECT_EQ(one["fingerprint"].asString(), fingerprint);
    EXPECT_EQ(one["message"].asString(), "something broke");
}

// A dropped filter answers a question nobody asked, and the whole page looks like the answer.
TEST(CrashReportEndpointTest, AFingerprintThatIsNotOneIsRefusedRatherThanIgnored) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = harness().createSessionWithRole(uniqueEmail("crashadmin"), "admin");

    EXPECT_EQ(harness()
                  .adminGet("/admin/api/crashes/reports?fingerprint=nonsense", tokenOf(session))
                  ->statusCode(),
              drogon::k422UnprocessableEntity);
}

TEST(CrashReportEndpointTest, AnUnknownReportIsMissing) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = harness().createSessionWithRole(uniqueEmail("crashadmin"), "admin");

    EXPECT_EQ(harness()
                  .adminGet("/admin/api/crashes/reports/00000000-0000-4000-8000-000000000000",
                            tokenOf(session))
                  ->statusCode(),
              drogon::k404NotFound);
    EXPECT_EQ(
        harness().adminGet("/admin/api/crashes/reports/not-a-uuid", tokenOf(session))->statusCode(),
        drogon::k404NotFound);
}

// ---------------------------------------------------------------------------
// What the client is told before it tries
// ---------------------------------------------------------------------------

TEST(CrashReportEndpointTest, CapabilitiesSayWhetherReportsAreAcceptedAndHowBig) {
    LAUNCHER_REQUIRE_DATABASE();

    const auto document = bodyOf(harness().get("/api/v1/capabilities"));

    ASSERT_TRUE(document.isMember("crashReports"));
    EXPECT_TRUE(document["crashReports"]["enabled"].asBool());
    EXPECT_GT(document["crashReports"]["maxStackLength"].asInt64(), 0);
}

} // namespace
