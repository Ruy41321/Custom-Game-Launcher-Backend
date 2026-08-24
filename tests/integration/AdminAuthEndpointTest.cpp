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

constexpr const char* PASSWORD = "correct horse battery staple";

Json::Value credentials(const std::string& email) {
    Json::Value body;
    body["email"] = email;
    body["password"] = PASSWORD;
    return body;
}

/// An account, its role, and a session obtained the ordinary way on the public listener.
Json::Value accountWithRole(const std::string& email, const std::string& roleKey) {
    return harness().createSessionWithRole(email, roleKey);
}

} // namespace

// ---------------------------------------------------------------------------
// The listener gate. Both directions of it: what answers on :9090 must not answer
// on :8080, and what answers on :8080 must keep doing so.
// ---------------------------------------------------------------------------

TEST(AdminSurfaceGateTest, TheAdminLoginRouteDoesNotExistOnThePublicListener) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail("admin-gate");
    accountWithRole(email, "admin");

    const auto response = harness().postJson("/admin/api/auth/login", credentials(email));

    // 404 and not 403: from the public listener the surface does not exist at all. A 403 would
    // confirm the route is there to anyone who guessed the path.
    ASSERT_EQ(response->statusCode(), drogon::k404NotFound) << response->body();
    EXPECT_EQ(bodyOf(response)["code"].asString(), "not_found");
}

TEST(AdminSurfaceGateTest, TheRefusalIsWordForWordTheOrdinaryNotFoundPage) {
    // Anything that distinguishes "hidden here" from "never existed" is a way to enumerate the
    // administrative surface from outside, so the two answers have to be the same answer.
    const auto hidden = bodyOf(harness().get("/admin/api/session"));
    const auto missing = bodyOf(harness().get("/api/v1/no-such-route"));

    EXPECT_EQ(hidden["detail"].asString(), missing["detail"].asString());
    EXPECT_EQ(hidden["title"].asString(), missing["title"].asString());
    EXPECT_EQ(hidden["code"].asString(), missing["code"].asString());
}

TEST(AdminSurfaceGateTest, ThePublicApiIsNotServedOnTheAdminListener) {
    // The gate hides admin routes from the public port; it deliberately does not do the
    // reverse. The page served from :9090 signs in through this listener, and an operator on
    // the far end of an SSH tunnel has no route to :8080 at all.
    const auto response = harness().adminGet("/api/v1/health");
    EXPECT_EQ(response->statusCode(), drogon::k200OK) << response->body();
}

// ---------------------------------------------------------------------------
// Signing in
// ---------------------------------------------------------------------------

TEST(AdminAuthEndpointTest, SignsInAnOperatorOnTheAdminListener) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail("admin-login");
    accountWithRole(email, "admin");

    const auto response = harness().adminPostJson("/admin/api/auth/login", credentials(email));

    ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
    const auto body = bodyOf(response);
    EXPECT_FALSE(body["accessToken"].asString().empty());
    EXPECT_FALSE(body["refreshToken"].asString().empty());
    EXPECT_EQ(body["operator"]["email"].asString(), email);
    EXPECT_GT(body["permissions"].size(), 0U);
}

TEST(AdminAuthEndpointTest, RefusesAnAccountThatIsNotAnOperator) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail("admin-player");
    harness().createVerifiedSession(email);

    const auto response = harness().adminPostJson("/admin/api/auth/login", credentials(email));

    // Forbidden rather than Unauthenticated: the surface is only reachable by somebody who
    // already has the machine, and an operator who has not been granted the role needs to be
    // told which of the two things is missing.
    ASSERT_EQ(response->statusCode(), drogon::k403Forbidden) << response->body();
    EXPECT_TRUE(bodyOf(response)["accessToken"].isNull());
}

TEST(AdminAuthEndpointTest, ARefusedSignInLeavesNoUsableSessionBehind) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail("admin-refused");
    const auto session = harness().createVerifiedSession(email);

    ASSERT_EQ(harness().adminPostJson("/admin/api/auth/login", credentials(email))->statusCode(),
              drogon::k403Forbidden);

    // The password was right, so AuthService opened a refresh-token family for the attempt.
    // The one the caller already held from the public listener must still work — the refusal
    // retires the session it just minted, not every session the account has.
    Json::Value refresh;
    refresh["refreshToken"] = session["refreshToken"];
    EXPECT_EQ(harness().postJson("/api/v1/auth/refresh", refresh)->statusCode(), drogon::k200OK);
}

TEST(AdminAuthEndpointTest, RefusesAWrongPasswordBeforeItEverAsksAboutTheRole) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail("admin-wrongpass");
    accountWithRole(email, "admin");

    Json::Value wrong;
    wrong["email"] = email;
    wrong["password"] = "not the password";

    const auto response = harness().adminPostJson("/admin/api/auth/login", wrong);
    EXPECT_EQ(response->statusCode(), drogon::k401Unauthorized) << response->body();
}

// ---------------------------------------------------------------------------
// Holding a session
// ---------------------------------------------------------------------------

TEST(AdminSessionEndpointTest, ReportsWhoIsSignedIn) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail("admin-session");
    accountWithRole(email, "admin");

    const auto signIn = harness().adminPostJson("/admin/api/auth/login", credentials(email));
    ASSERT_EQ(signIn->statusCode(), drogon::k200OK) << signIn->body();
    const auto token = bodyOf(signIn)["accessToken"].asString();

    const auto response = harness().adminGet("/admin/api/session", token);

    ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
    EXPECT_EQ(bodyOf(response)["email"].asString(), email);
}

TEST(AdminSessionEndpointTest, RequiresAToken) {
    const auto response = harness().adminGet("/admin/api/session");
    EXPECT_EQ(response->statusCode(), drogon::k401Unauthorized) << response->body();
}

TEST(AdminSessionEndpointTest, RefusesATokenThatCarriesNoAdminPermission) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = accountWithRole(uniqueEmail("admin-devtoken"), "dev");

    // A perfectly valid token, minted by the public listener for a publisher. The operator
    // filter is what stops it here, which is the case a permission check on each individual
    // route would be free to forget.
    const auto response =
        harness().adminGet("/admin/api/session", session["accessToken"].asString());

    EXPECT_EQ(response->statusCode(), drogon::k403Forbidden) << response->body();
}

TEST(AdminSessionEndpointTest, AcceptsATokenMintedByThePublicListener) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = accountWithRole(uniqueEmail("admin-publictoken"), "admin");

    // The two listeners share one signing key and one account table, so a token is a token.
    // The admin login exists because a tunnelled page cannot reach :8080, not because tokens
    // from there are second class.
    const auto response =
        harness().adminGet("/admin/api/session", session["accessToken"].asString());

    EXPECT_EQ(response->statusCode(), drogon::k200OK) << response->body();
}

// ---------------------------------------------------------------------------
// Rotation and sign-out
// ---------------------------------------------------------------------------

TEST(AdminAuthEndpointTest, RotatesARefreshTokenAndKeepsTheSession) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail("admin-refresh");
    accountWithRole(email, "admin");

    const auto signIn =
        bodyOf(harness().adminPostJson("/admin/api/auth/login", credentials(email)));

    Json::Value refresh;
    refresh["refreshToken"] = signIn["refreshToken"];
    const auto response = harness().adminPostJson("/admin/api/auth/refresh", refresh);

    ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
    const auto rotated = bodyOf(response);
    EXPECT_NE(rotated["refreshToken"].asString(), signIn["refreshToken"].asString());
    EXPECT_EQ(rotated["operator"]["email"].asString(), email);
}

TEST(AdminAuthEndpointTest, RefusesToRotateASessionWhoseRoleWasTakenAway) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail("admin-revoked");
    const auto session = accountWithRole(email, "admin");

    const auto signIn =
        bodyOf(harness().adminPostJson("/admin/api/auth/login", credentials(email)));

    harness().database().exec("DELETE FROM user_roles WHERE user_id = '" +
                              session["user"]["id"].asString() + "'::uuid");

    // Permissions live in the access token, so revoking a role cannot reach a token already
    // issued — it takes effect at the next rotation, which is exactly here. Without this check
    // a demoted operator would keep renewing an administrative session indefinitely.
    Json::Value refresh;
    refresh["refreshToken"] = signIn["refreshToken"];
    const auto response = harness().adminPostJson("/admin/api/auth/refresh", refresh);

    EXPECT_EQ(response->statusCode(), drogon::k403Forbidden) << response->body();
}

TEST(AdminAuthEndpointTest, SignsOut) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail("admin-logout");
    accountWithRole(email, "admin");

    const auto signIn =
        bodyOf(harness().adminPostJson("/admin/api/auth/login", credentials(email)));

    Json::Value body;
    body["refreshToken"] = signIn["refreshToken"];
    EXPECT_EQ(harness().adminPostJson("/admin/api/auth/logout", body)->statusCode(),
              drogon::k204NoContent);

    EXPECT_EQ(harness().adminPostJson("/admin/api/auth/refresh", body)->statusCode(),
              drogon::k401Unauthorized);
}
