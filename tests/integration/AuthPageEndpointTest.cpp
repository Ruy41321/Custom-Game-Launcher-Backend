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

std::string uniqueEmail() {
    static std::atomic<int> counter{0};
    return "page-user" + std::to_string(counter.fetch_add(1)) + "@example.test";
}

Json::Value registerPayload(const std::string& email) {
    Json::Value body;
    body["email"] = email;
    body["password"] = "correct horse battery staple";
    body["displayName"] = "Test User";
    return body;
}

// The pages are what makes a link in a message something a person can follow: every
// authentication route on this server is a JSON POST, and an inbox opens URLs with a browser.
TEST(AuthPageEndpointTest, ServesTheVerificationPageAsHtml) {
    const auto response = harness().get("/verify-email?token=whatever");

    ASSERT_NE(response, nullptr);
    EXPECT_EQ(response->statusCode(), drogon::k200OK);
    EXPECT_NE(response->getHeader("Content-Type").find("text/html"), std::string::npos);
    EXPECT_NE(response->body().find("/api/v1/auth/verify-email"), std::string::npos)
        << "the page has to call the route it exists in front of";
}

TEST(AuthPageEndpointTest, ServesThePasswordResetPageAsHtml) {
    const auto response = harness().get("/password-reset?token=whatever");

    ASSERT_NE(response, nullptr);
    EXPECT_EQ(response->statusCode(), drogon::k200OK);
    EXPECT_NE(response->body().find("/api/v1/auth/password-reset/confirm"), std::string::npos);
}

// The URL carries a single-use token, so neither a cache nor a referrer may keep it, and the
// page may load nothing from anywhere else.
TEST(AuthPageEndpointTest, StatesItsOwnPolicyAndRefusesToBeStored) {
    const auto response = harness().get("/verify-email");

    ASSERT_NE(response, nullptr);
    EXPECT_EQ(response->getHeader("Cache-Control"), "no-store");
    EXPECT_EQ(response->getHeader("Referrer-Policy"), "no-referrer");
    const auto policy = response->getHeader("Content-Security-Policy");
    EXPECT_NE(policy.find("default-src 'none'"), std::string::npos);
    EXPECT_NE(policy.find("connect-src 'self'"), std::string::npos);
    EXPECT_NE(policy.find("frame-ancestors 'none'"), std::string::npos);
}

// A mail provider's link scanner fetches every URL in a message. A page that confirmed on load
// would therefore spend the token before its owner clicked, and they would be shown "this link
// is invalid" for having done nothing wrong.
TEST(AuthPageEndpointTest, OpeningThePageConsumesNothing) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail();
    ASSERT_EQ(harness().postJson("/api/v1/auth/register", registerPayload(email))->statusCode(),
              drogon::k201Created);
    const auto token = harness().tokenMailedTo(email);
    ASSERT_FALSE(token.empty());

    ASSERT_EQ(harness().get("/verify-email?token=" + token)->statusCode(), drogon::k200OK);
    ASSERT_EQ(harness().get("/verify-email?token=" + token)->statusCode(), drogon::k200OK);

    Json::Value verify;
    verify["token"] = token;
    EXPECT_EQ(harness().postJson("/api/v1/auth/verify-email", verify)->statusCode(), drogon::k200OK)
        << "the token must still be good after the page has been opened twice";
}

} // namespace
