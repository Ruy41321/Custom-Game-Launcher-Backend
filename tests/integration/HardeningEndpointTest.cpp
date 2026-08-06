#include <gtest/gtest.h>

#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <atomic>
#include <chrono>
#include <string>

#include "integration/AppHarness.h"

namespace {

using launcher::testing::AppHarness;
using launcher::testing::ScopedAccountRateLimit;

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

void expectSecurityHeaders(const drogon::HttpResponsePtr& response) {
    EXPECT_EQ(response->getHeader("X-Content-Type-Options"), "nosniff");
    EXPECT_EQ(response->getHeader("X-Frame-Options"), "DENY");
    EXPECT_EQ(response->getHeader("Referrer-Policy"), "no-referrer");
    EXPECT_NE(response->getHeader("Content-Security-Policy").find("frame-ancestors 'none'"),
              std::string::npos);
}

} // namespace

// ---------------------------------------------------------------------------
// The headers every response carries
// ---------------------------------------------------------------------------

TEST(HardeningEndpointTest, EverySuccessfulResponseDescribesItself) {
    expectSecurityHeaders(harness().get("/api/v1/health"));
}

TEST(HardeningEndpointTest, AnEnvelopeARouteNeverAnsweredCarriesThemToo) {
    // A filter that refuses a request answers it itself, and such a response never reaches
    // post-handling advice — so every 401, 403 and throttle on this server would have been the
    // one kind of response with nothing on it. They are stamped where the envelope is built
    // instead, which is the single place all of them pass through.
    const auto response = harness().get("/api/v1/auth/me");

    ASSERT_EQ(response->statusCode(), drogon::k401Unauthorized);
    expectSecurityHeaders(response);
}

TEST(HardeningEndpointTest, TheCachedNotFoundPageCarriesThemAsWell) {
    // Drogon hands one shared object to every request that misses, so this response cannot be
    // stamped by the advice that stamps the others; it is built with its headers instead.
    // Asked for twice on purpose: a second answer proves the shared object was not mutated
    // into something different by the first.
    expectSecurityHeaders(harness().get("/api/v1/no-such-route"));
    expectSecurityHeaders(harness().get("/api/v1/no-such-route"));
}

TEST(HardeningEndpointTest, StatesTransportSecurityWhenTheDeploymentSaysItIsBehindTls) {
    EXPECT_FALSE(harness().get("/api/v1/health")->getHeader("Strict-Transport-Security").empty());
}

TEST(HardeningEndpointTest, LeavesTheConsolesOwnPolicyAlone) {
    // The API's policy describes something that is never a page. The console *is* one, and its
    // policy is the stricter of the two: replacing it here would loosen the only surface on
    // this server where a content policy does any work at all.
    const auto policy = harness().adminGet("/admin")->getHeader("Content-Security-Policy");

    EXPECT_NE(policy.find("connect-src 'self'"), std::string::npos);
    EXPECT_NE(policy.find("script-src 'unsafe-inline'"), std::string::npos);
}

// ---------------------------------------------------------------------------
// The ceiling an account has, whatever address it calls from
// ---------------------------------------------------------------------------

TEST(HardeningEndpointTest, ThrottlesAnAccountThatKeepsAsking) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = harness().createVerifiedSession(uniqueEmail("throttled"));
    const auto token = session["accessToken"].asString();

    // Narrowed rather than out-run: the bucket refills continuously, so how many requests empty
    // a wide one is a function of how fast the machine answers them — which is the shape that
    // passed here and failed in CI once already.
    const ScopedAccountRateLimit limit(3, std::chrono::seconds{60});

    EXPECT_EQ(harness().get("/api/v1/auth/me", token)->statusCode(), drogon::k200OK);
    EXPECT_EQ(harness().get("/api/v1/auth/me", token)->statusCode(), drogon::k200OK);
    EXPECT_EQ(harness().get("/api/v1/auth/me", token)->statusCode(), drogon::k200OK);

    const auto refused = harness().get("/api/v1/auth/me", token);
    ASSERT_EQ(refused->statusCode(), drogon::k429TooManyRequests) << refused->body();
    EXPECT_EQ(bodyOf(refused)["code"].asString(), "rate_limited");
    // The client is told when to come back rather than left to guess, which is what makes a
    // throttle something a launcher can obey instead of something it retries into.
    EXPECT_FALSE(refused->getHeader("Retry-After").empty());
}

TEST(HardeningEndpointTest, OneAccountRunningOutDoesNotTouchAnother) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto busy = harness().createVerifiedSession(uniqueEmail("busy"));
    const auto quiet = harness().createVerifiedSession(uniqueEmail("quiet"));
    const auto busyToken = busy["accessToken"].asString();
    const auto quietToken = quiet["accessToken"].asString();

    const ScopedAccountRateLimit limit(2, std::chrono::seconds{60});

    EXPECT_EQ(harness().get("/api/v1/auth/me", busyToken)->statusCode(), drogon::k200OK);
    EXPECT_EQ(harness().get("/api/v1/auth/me", busyToken)->statusCode(), drogon::k200OK);
    ASSERT_EQ(harness().get("/api/v1/auth/me", busyToken)->statusCode(),
              drogon::k429TooManyRequests);

    // The bucket is keyed on the account. Somebody else's exhausted allowance is not a reason
    // to refuse this one, which is the entire difference between this and the address bucket.
    EXPECT_EQ(harness().get("/api/v1/auth/me", quietToken)->statusCode(), drogon::k200OK);
}

TEST(HardeningEndpointTest, ItAppliesToEveryAuthenticatedRouteRatherThanAChosenFew) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = harness().createVerifiedSession(uniqueEmail("everywhere"));
    const auto token = session["accessToken"].asString();

    const ScopedAccountRateLimit limit(1, std::chrono::seconds{60});

    EXPECT_EQ(harness().get("/api/v1/auth/me", token)->statusCode(), drogon::k200OK);

    // A different route entirely, and the same exhausted allowance: the ceiling lives in the
    // filter every authenticated route already runs through, so no route can be added without
    // one.
    EXPECT_EQ(harness().get("/api/v1/library", token)->statusCode(), drogon::k429TooManyRequests);
}

TEST(HardeningEndpointTest, ItDoesNotStandBetweenAnAnonymousCallerAndAnUnauthenticatedRoute) {
    const ScopedAccountRateLimit limit(1, std::chrono::seconds{60});

    // Nothing to key on, so nothing to spend. `/capabilities` is the route a launcher reads
    // before it has a session at all, and a ceiling on accounts that could refuse it would be
    // a launcher unable to learn how to sign in.
    EXPECT_EQ(harness().get("/api/v1/capabilities")->statusCode(), drogon::k200OK);
    EXPECT_EQ(harness().get("/api/v1/capabilities")->statusCode(), drogon::k200OK);
    EXPECT_EQ(harness().get("/api/v1/capabilities")->statusCode(), drogon::k200OK);
}

// ---------------------------------------------------------------------------
// What a caller with no account may send
// ---------------------------------------------------------------------------

TEST(HardeningEndpointTest, RefusesADocumentSizedBodyFromACallerWithNoToken) {
    // Sign-in, a refresh and a crash report are all small, and nothing anonymous on this server
    // has a reason to send a document. Until this existed, every route inherited the *upload
    // chunk* limit, so an unauthenticated caller could have a megabyte parsed by asking.
    Json::Value login;
    login["email"] = "somebody@example.test";
    login["password"] = std::string(70 * 1024, 'x');

    const auto response = harness().postJson("/api/v1/auth/login", login);

    ASSERT_EQ(response->statusCode(), drogon::k413RequestEntityTooLarge) << response->body();
    EXPECT_EQ(bodyOf(response)["code"].asString(), "quota_exceeded");
}

TEST(HardeningEndpointTest, ACrashReportOfAnOrdinarySizeStillGoesThrough) {
    LAUNCHER_REQUIRE_DATABASE();

    // The cap has to sit above every legitimate anonymous body, and the largest of those is a
    // crash report carrying a full stack trace. This is the assertion that would catch a cap
    // set below one.
    Json::Value report;
    report["kind"] = "unhandled";
    report["occurredAt"] = "2026-08-06T12:00:00Z";
    report["exceptionType"] = "System.IO.IOException";
    report["message"] = "the disk is full";
    report["stackTrace"] = std::string(15000, 'f');

    EXPECT_EQ(harness().postJson("/api/v1/crash-reports", report)->statusCode(),
              drogon::k202Accepted);
}

TEST(HardeningEndpointTest, ASignedInCallerMaySendADocument) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = harness().createVerifiedSession(uniqueEmail("publisher"));
    const auto token = session["accessToken"].asString();

    Json::Value game;
    game["title"] = "Oversized";
    game["summary"] = std::string(70 * 1024, 'y');

    // Refused for what it says rather than for how big it is: a manifest legitimately runs to
    // megabytes, so the anonymous cap must not be the one answering here.
    const auto response = harness().postJson("/api/v1/games", game, token);

    EXPECT_NE(response->statusCode(), drogon::k413RequestEntityTooLarge) << response->body();
}
