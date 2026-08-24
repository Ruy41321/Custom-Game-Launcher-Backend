#include <gtest/gtest.h>

#include <drogon/HttpResponse.h>

#include <string>

#include "integration/AppHarness.h"

namespace {

using launcher::testing::AppHarness;

AppHarness& harness() {
    return *AppHarness::current();
}

} // namespace

TEST(AdminUiEndpointTest, ServesTheConsoleOnTheAdminListener) {
    const auto response = harness().adminGet("/admin");

    ASSERT_EQ(response->statusCode(), drogon::k200OK);
    EXPECT_NE(response->body().find("<title>Custom Game Launcher"), std::string::npos);
    EXPECT_EQ(response->getHeader("Content-Type").find("text/html"), 0U);
}

TEST(AdminUiEndpointTest, ServesItWithTheTrailingSlashToo) {
    EXPECT_EQ(harness().adminGet("/admin/")->statusCode(), drogon::k200OK);
}

TEST(AdminUiEndpointTest, DoesNotExistOnThePublicListener) {
    EXPECT_EQ(harness().get("/admin")->statusCode(), drogon::k404NotFound);
    EXPECT_EQ(harness().get("/admin/")->statusCode(), drogon::k404NotFound);
}

TEST(AdminUiEndpointTest, NeedsNoTokenToLoad) {
    // Deliberate: the page has to load before anybody can sign in. It carries no data — every
    // number on it comes from an endpoint that does check — so an unauthenticated caller on
    // the loopback listener gets a login form and nothing else.
    const auto response = harness().adminGet("/admin");

    ASSERT_EQ(response->statusCode(), drogon::k200OK);
    EXPECT_NE(response->body().find("Sign in"), std::string::npos);
}

// The console is embedded at build time, so "the page a deployment serves" and "the file in
// the repository" are the same bytes — which makes this the cheapest way to catch a control
// that was written and never wired to its route.
TEST(AdminUiEndpointTest, OffersTheOneTimePasswordAgainstTheRouteThatServesIt) {
    const auto body = harness().adminGet("/admin")->body();

    EXPECT_NE(body.find("/temporary-password"), std::string::npos);
    EXPECT_NE(body.find("Set a temporary password"), std::string::npos);

    // It is shown once and never fetched again, so the page must say so where somebody
    // reading it will see it before they close the row.
    EXPECT_NE(body.find("cannot show it again"), std::string::npos);
}

TEST(AdminUiEndpointTest, RefusesToLoadAnythingFromAnywhereElse) {
    const auto policy = harness().adminGet("/admin")->getHeader("Content-Security-Policy");

    // The page is self-contained, so the policy that describes it is the strictest one there
    // is. A future edit reaching for a CDN then fails loudly in the browser instead of quietly
    // adding a third party to an operator console.
    ASSERT_FALSE(policy.empty());
    EXPECT_NE(policy.find("default-src 'none'"), std::string::npos);
    EXPECT_NE(policy.find("frame-ancestors 'none'"), std::string::npos);
    EXPECT_NE(policy.find("connect-src 'self'"), std::string::npos);
}

TEST(AdminUiEndpointTest, IsNeverCached) {
    // An operator returning after a deployment must not be handed a console built against a
    // different API.
    EXPECT_EQ(harness().adminGet("/admin")->getHeader("Cache-Control"), "no-store");
    EXPECT_EQ(harness().adminGet("/admin")->getHeader("X-Content-Type-Options"), "nosniff");
}

TEST(AdminUiEndpointTest, KeepsTheTokenOutOfPersistentStorage) {
    // Asserted on the served bytes rather than left to review: this page is reached over a
    // tunnel from somebody's desktop, and a token that survives the tab outlives the reason it
    // was issued.
    const auto body = harness().adminGet("/admin")->body();

    EXPECT_EQ(body.find("localStorage"), std::string::npos);
    EXPECT_EQ(body.find("sessionStorage"), std::string::npos);
    EXPECT_EQ(body.find("document.cookie"), std::string::npos);
}

TEST(AdminUiEndpointTest, ReachesCrashReportsFromTheNavigation) {
    const auto body = harness().adminGet("/admin")->body();

    EXPECT_NE(body.find("data-tab=\"crashes\""), std::string::npos);
    EXPECT_NE(body.find("id=\"tab-crashes\""), std::string::npos);
}

TEST(AdminUiEndpointTest, CallsEveryCrashRouteTheServerExposes) {
    // Three routes because an operator asks two questions, and the answer to the second is a
    // report in full. A screen that only listed the groups would leave the other two answering
    // nobody, which is what this page was for a day.
    const auto body = harness().adminGet("/admin")->body();

    EXPECT_NE(body.find("/admin/api/crashes?page="), std::string::npos);
    EXPECT_NE(body.find("/admin/api/crashes/reports?"), std::string::npos);
    EXPECT_NE(body.find("/admin/api/crashes/reports/\""), std::string::npos);
}

TEST(AdminUiEndpointTest, NarrowsTheReportsByFingerprintRatherThanByAccount) {
    // There is no account to narrow by, and there is no column for one. Asserted on the served
    // bytes so a later edit cannot quietly add a filter the server would have to grow a field
    // for.
    const auto body = harness().adminGet("/admin")->body();

    EXPECT_NE(body.find("parameters.set(\"fingerprint\""), std::string::npos);
}

TEST(AdminUiEndpointTest, LoadsNothingFromTheNetworkBesidesItsOwnApi) {
    // The CSP is the enforcement; this is the check that the page does not need it relaxed.
    const auto body = harness().adminGet("/admin")->body();

    EXPECT_EQ(body.find("https://"), std::string::npos);
    EXPECT_EQ(body.find("http://"), std::string::npos);
}
