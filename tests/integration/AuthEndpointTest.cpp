#include <gtest/gtest.h>

#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>

#include "integration/AppHarness.h"

namespace {

using launcher::testing::AppHarness;
using launcher::testing::ScopedAuthRateLimit;
using launcher::testing::ScopedMailRateLimit;

constexpr const char* PASSWORD = "correct horse battery staple";

AppHarness& harness() {
    return *AppHarness::current();
}

/// Makes every send fail for the length of a scope, whichever way the scope ends — the sender
/// is shared by the whole binary, so an assertion that stopped a test early must not leave it
/// refusing for everybody after it.
struct ScopedMailFailure {
    ScopedMailFailure() { harness().mail().refuseEverything(true); }

    ~ScopedMailFailure() { harness().mail().refuseEverything(false); }

    ScopedMailFailure(const ScopedMailFailure&) = delete;
    ScopedMailFailure& operator=(const ScopedMailFailure&) = delete;
};

/// Each test registers its own account, so tests never collide over one shared fixture row.
std::string uniqueEmail() {
    static std::atomic<int> counter{0};
    return "user" + std::to_string(counter.fetch_add(1)) + "@example.test";
}

Json::Value bodyOf(const drogon::HttpResponsePtr& response) {
    EXPECT_NE(response, nullptr);
    const auto json = response->getJsonObject();
    EXPECT_NE(json, nullptr) << "expected a JSON body";
    return json == nullptr ? Json::Value{} : *json;
}

Json::Value registerPayload(const std::string& email) {
    Json::Value body;
    body["email"] = email;
    body["password"] = PASSWORD;
    body["displayName"] = "Test User";
    return body;
}

Json::Value credentials(const std::string& email, const std::string& password = PASSWORD) {
    Json::Value body;
    body["email"] = email;
    body["password"] = password;
    return body;
}

/// Registers, verifies the address and logs in. Returns the session body.
Json::Value createVerifiedSession(const std::string& email) {
    const auto registered = harness().postJson("/api/v1/auth/register", registerPayload(email));
    EXPECT_EQ(registered->statusCode(), drogon::k201Created);

    Json::Value verify;
    verify["token"] = harness().tokenMailedTo(email);
    EXPECT_EQ(harness().postJson("/api/v1/auth/verify-email", verify)->statusCode(),
              drogon::k200OK);

    const auto loggedIn = harness().postJson("/api/v1/auth/login", credentials(email));
    EXPECT_EQ(loggedIn->statusCode(), drogon::k200OK);
    return bodyOf(loggedIn);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

TEST(AuthEndpointTest, RegistersAnAccount) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail();

    const auto response = harness().postJson("/api/v1/auth/register", registerPayload(email));

    ASSERT_EQ(response->statusCode(), drogon::k201Created);
    const auto body = bodyOf(response);
    EXPECT_EQ(body["user"]["email"].asString(), email);
    EXPECT_FALSE(body["user"]["id"].asString().empty());
    EXPECT_FALSE(body["user"]["emailVerified"].asBool());
    EXPECT_TRUE(body["emailVerificationRequired"].asBool());
    // The response must never echo the password back in any form.
    EXPECT_EQ(response->body().find(PASSWORD), std::string::npos);
}

TEST(AuthEndpointTest, RejectsADuplicateAddressWithAConflict) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail();
    ASSERT_EQ(harness().postJson("/api/v1/auth/register", registerPayload(email))->statusCode(),
              drogon::k201Created);

    const auto response = harness().postJson("/api/v1/auth/register", registerPayload(email));

    EXPECT_EQ(response->statusCode(), drogon::k409Conflict);
    EXPECT_EQ(bodyOf(response)["code"].asString(), "conflict");
}

TEST(AuthEndpointTest, RejectsInvalidRegistrationInput) {
    LAUNCHER_REQUIRE_DATABASE();

    Json::Value weakPassword = registerPayload(uniqueEmail());
    weakPassword["password"] = "short";
    const auto response = harness().postJson("/api/v1/auth/register", weakPassword);

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
    EXPECT_EQ(bodyOf(response)["code"].asString(), "invalid_input");
}

TEST(AuthEndpointTest, RejectsAMissingField) {
    LAUNCHER_REQUIRE_DATABASE();

    Json::Value incomplete;
    incomplete["email"] = uniqueEmail();

    const auto response = harness().postJson("/api/v1/auth/register", incomplete);

    EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
    EXPECT_NE(bodyOf(response)["detail"].asString().find("password"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Email verification and login
// ---------------------------------------------------------------------------

TEST(AuthEndpointTest, RefusesLoginUntilTheAddressIsVerified) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail();
    ASSERT_EQ(harness().postJson("/api/v1/auth/register", registerPayload(email))->statusCode(),
              drogon::k201Created);

    const auto response = harness().postJson("/api/v1/auth/login", credentials(email));

    EXPECT_EQ(response->statusCode(), drogon::k403Forbidden);
}

TEST(AuthEndpointTest, VerifiesTheAddressAndThenIssuesASession) {
    LAUNCHER_REQUIRE_DATABASE();

    const auto session = createVerifiedSession(uniqueEmail());

    EXPECT_FALSE(session["accessToken"].asString().empty());
    EXPECT_FALSE(session["refreshToken"].asString().empty());
    EXPECT_EQ(session["tokenType"].asString(), "Bearer");
    EXPECT_GT(session["expiresIn"].asInt64(), 0);
    EXPECT_TRUE(session["user"]["emailVerified"].asBool());
}

// The default role must actually be granted, or a fresh account can do nothing.
TEST(AuthEndpointTest, GrantsThePlayerPermissionsOnRegistration) {
    LAUNCHER_REQUIRE_DATABASE();

    const auto session = createVerifiedSession(uniqueEmail());

    std::vector<std::string> permissions;
    for (const auto& permission : session["permissions"]) {
        permissions.push_back(permission.asString());
    }

    EXPECT_NE(std::find(permissions.begin(), permissions.end(), "library.read"), permissions.end());
    EXPECT_NE(std::find(permissions.begin(), permissions.end(), "game.download"),
              permissions.end());
    EXPECT_EQ(std::find(permissions.begin(), permissions.end(), "admin.users.manage"),
              permissions.end())
        << "a new account must not receive admin permissions";
}

TEST(AuthEndpointTest, AVerificationTokenCannotBeReplayed) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail();
    const auto registered = harness().postJson("/api/v1/auth/register", registerPayload(email));
    ASSERT_EQ(registered->statusCode(), drogon::k201Created);

    Json::Value verify;
    verify["token"] = harness().tokenMailedTo(email);
    ASSERT_EQ(harness().postJson("/api/v1/auth/verify-email", verify)->statusCode(),
              drogon::k200OK);

    EXPECT_EQ(harness().postJson("/api/v1/auth/verify-email", verify)->statusCode(),
              drogon::k422UnprocessableEntity);
}

TEST(AuthEndpointTest, GivesTheSameAnswerForAnUnknownEmailAndAWrongPassword) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail();
    createVerifiedSession(email);

    const auto unknown =
        harness().postJson("/api/v1/auth/login", credentials("nobody@example.test"));
    const auto wrong =
        harness().postJson("/api/v1/auth/login", credentials(email, "a totally wrong password"));

    EXPECT_EQ(unknown->statusCode(), drogon::k401Unauthorized);
    EXPECT_EQ(wrong->statusCode(), drogon::k401Unauthorized);
    EXPECT_EQ(bodyOf(unknown)["detail"].asString(), bodyOf(wrong)["detail"].asString());
}

// ---------------------------------------------------------------------------
// Authenticated routes
// ---------------------------------------------------------------------------

TEST(AuthEndpointTest, ReturnsTheCurrentUserForAValidBearerToken) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail();
    const auto session = createVerifiedSession(email);

    const auto response = harness().get("/api/v1/auth/me", session["accessToken"].asString());

    ASSERT_EQ(response->statusCode(), drogon::k200OK);
    EXPECT_EQ(bodyOf(response)["email"].asString(), email);
}

TEST(AuthEndpointTest, RefusesAnAuthenticatedRouteWithoutACredential) {
    LAUNCHER_REQUIRE_DATABASE();

    for (const auto* token : {"", "not-a-jwt", "a.b.c"}) {
        const auto response = harness().get("/api/v1/auth/me", token);
        EXPECT_EQ(response->statusCode(), drogon::k401Unauthorized) << "token: " << token;
        EXPECT_EQ(bodyOf(response)["code"].asString(), "unauthenticated");
    }
}

// ---------------------------------------------------------------------------
// Refresh rotation
// ---------------------------------------------------------------------------

TEST(AuthEndpointTest, RotatesTheRefreshTokenAndInvalidatesTheOldOne) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = createVerifiedSession(uniqueEmail());

    Json::Value refresh;
    refresh["refreshToken"] = session["refreshToken"].asString();
    const auto rotated = harness().postJson("/api/v1/auth/refresh", refresh);

    ASSERT_EQ(rotated->statusCode(), drogon::k200OK);
    const auto rotatedBody = bodyOf(rotated);
    EXPECT_NE(rotatedBody["refreshToken"].asString(), session["refreshToken"].asString());
    EXPECT_FALSE(rotatedBody["accessToken"].asString().empty());

    // The new token works.
    Json::Value again;
    again["refreshToken"] = rotatedBody["refreshToken"].asString();
    EXPECT_EQ(harness().postJson("/api/v1/auth/refresh", again)->statusCode(), drogon::k200OK);
}

// The central anti-theft rule, end to end: replaying a rotated token burns the family.
TEST(AuthEndpointTest, ReplayingARotatedTokenKillsTheWholeFamily) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = createVerifiedSession(uniqueEmail());

    Json::Value first;
    first["refreshToken"] = session["refreshToken"].asString();
    const auto rotated = harness().postJson("/api/v1/auth/refresh", first);
    ASSERT_EQ(rotated->statusCode(), drogon::k200OK);
    const auto liveToken = bodyOf(rotated)["refreshToken"].asString();

    // Replay the already-rotated token.
    EXPECT_EQ(harness().postJson("/api/v1/auth/refresh", first)->statusCode(),
              drogon::k401Unauthorized);

    // The token issued by the legitimate rotation is revoked along with it.
    Json::Value survivor;
    survivor["refreshToken"] = liveToken;
    EXPECT_EQ(harness().postJson("/api/v1/auth/refresh", survivor)->statusCode(),
              drogon::k401Unauthorized);
}

TEST(AuthEndpointTest, RejectsAnUnknownRefreshToken) {
    LAUNCHER_REQUIRE_DATABASE();

    Json::Value refresh;
    refresh["refreshToken"] = "never-issued-by-anyone";

    EXPECT_EQ(harness().postJson("/api/v1/auth/refresh", refresh)->statusCode(),
              drogon::k401Unauthorized);
}

TEST(AuthEndpointTest, LogoutRevokesTheSession) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = createVerifiedSession(uniqueEmail());

    Json::Value token;
    token["refreshToken"] = session["refreshToken"].asString();
    ASSERT_EQ(harness().postJson("/api/v1/auth/logout", token)->statusCode(), drogon::k200OK);

    EXPECT_EQ(harness().postJson("/api/v1/auth/refresh", token)->statusCode(),
              drogon::k401Unauthorized);
}

// ---------------------------------------------------------------------------
// Password reset
// ---------------------------------------------------------------------------

TEST(AuthEndpointTest, ResetsThePasswordAndEndsExistingSessions) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail();
    const auto session = createVerifiedSession(email);

    Json::Value request;
    request["email"] = email;
    const auto requested = harness().postJson("/api/v1/auth/password-reset/request", request);
    ASSERT_EQ(requested->statusCode(), drogon::k200OK);

    Json::Value confirm;
    confirm["token"] = harness().tokenMailedTo(email);
    confirm["password"] = "an entirely different passphrase";
    ASSERT_EQ(harness().postJson("/api/v1/auth/password-reset/confirm", confirm)->statusCode(),
              drogon::k200OK);

    EXPECT_EQ(harness().postJson("/api/v1/auth/login", credentials(email))->statusCode(),
              drogon::k401Unauthorized)
        << "the old password must stop working";
    EXPECT_EQ(
        harness()
            .postJson("/api/v1/auth/login", credentials(email, "an entirely different passphrase"))
            ->statusCode(),
        drogon::k200OK);

    // A reset is the recovery path after a compromise, so old sessions must die with it.
    Json::Value oldSession;
    oldSession["refreshToken"] = session["refreshToken"].asString();
    EXPECT_EQ(harness().postJson("/api/v1/auth/refresh", oldSession)->statusCode(),
              drogon::k401Unauthorized);
}

// An unauthenticated endpoint that distinguishes known from unknown addresses is an
// account enumeration tool.
TEST(AuthEndpointTest, PasswordResetLooksIdenticalForAnUnknownAddress) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto known = uniqueEmail();
    createVerifiedSession(known);

    Json::Value knownRequest;
    knownRequest["email"] = known;
    Json::Value unknownRequest;
    unknownRequest["email"] = "definitely-not-registered@example.test";

    const auto knownResponse =
        harness().postJson("/api/v1/auth/password-reset/request", knownRequest);
    const auto unknownResponse =
        harness().postJson("/api/v1/auth/password-reset/request", unknownRequest);

    EXPECT_EQ(knownResponse->statusCode(), unknownResponse->statusCode());
    EXPECT_EQ(bodyOf(knownResponse)["status"].asString(),
              bodyOf(unknownResponse)["status"].asString());
    EXPECT_FALSE(harness().mail().lastTo("definitely-not-registered@example.test").has_value())
        << "the answer is identical, and so is the silence: nothing may be sent";
}

// ---------------------------------------------------------------------------
// Delivery
// ---------------------------------------------------------------------------

TEST(AuthEndpointTest, RegistrationMailsALinkAndNoResponseCarriesAToken) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail();

    const auto registered = harness().postJson("/api/v1/auth/register", registerPayload(email));

    ASSERT_EQ(registered->statusCode(), drogon::k201Created);
    const auto body = bodyOf(registered);
    EXPECT_TRUE(body["verificationEmailSent"].asBool());
    // The affordance is gone rather than moved: nothing in the response can be turned back
    // into a link, in any environment.
    EXPECT_FALSE(body.isMember("devEmailVerificationToken"));

    const auto message = harness().mail().lastTo(email);
    ASSERT_TRUE(message.has_value());
    EXPECT_NE(message->body.find("https://launcher.test/verify-email?token="), std::string::npos)
        << "the link has to be built from the configured origin, never from the Host header";
}

// The failure the whole feature exists to survive, end to end: the relay is down, the account
// is created anyway, its owner cannot sign in yet, and the resend route is the way out.
TEST(AuthEndpointTest, AnAccountSurvivesAMessageThatCouldNotBeSentAndIsRecoveredByAResend) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail();

    {
        const ScopedMailFailure relayIsDown;
        const auto registered = harness().postJson("/api/v1/auth/register", registerPayload(email));
        ASSERT_EQ(registered->statusCode(), drogon::k201Created);
        EXPECT_FALSE(bodyOf(registered)["verificationEmailSent"].asBool());
    }

    EXPECT_EQ(harness().postJson("/api/v1/auth/login", credentials(email))->statusCode(),
              drogon::k403Forbidden)
        << "the account exists and is waiting for an address it was never able to confirm";

    Json::Value resend;
    resend["email"] = email;
    ASSERT_EQ(harness().postJson("/api/v1/auth/verify-email/resend", resend)->statusCode(),
              drogon::k200OK);

    Json::Value verify;
    verify["token"] = harness().tokenMailedTo(email);
    ASSERT_FALSE(verify["token"].asString().empty());
    ASSERT_EQ(harness().postJson("/api/v1/auth/verify-email", verify)->statusCode(),
              drogon::k200OK);
    EXPECT_EQ(harness().postJson("/api/v1/auth/login", credentials(email))->statusCode(),
              drogon::k200OK);
}

TEST(AuthEndpointTest, ResendingLooksIdenticalForAnAddressNobodyRegistered) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto known = uniqueEmail();
    harness().postJson("/api/v1/auth/register", registerPayload(known));

    Json::Value knownRequest;
    knownRequest["email"] = known;
    Json::Value unknownRequest;
    unknownRequest["email"] = "never-registered@example.test";

    const auto knownResponse = harness().postJson("/api/v1/auth/verify-email/resend", knownRequest);
    const auto unknownResponse =
        harness().postJson("/api/v1/auth/verify-email/resend", unknownRequest);

    EXPECT_EQ(knownResponse->statusCode(), unknownResponse->statusCode());
    EXPECT_EQ(bodyOf(knownResponse)["status"].asString(),
              bodyOf(unknownResponse)["status"].asString());
    EXPECT_FALSE(harness().mail().lastTo("never-registered@example.test").has_value());
}

// ---------------------------------------------------------------------------
// Rate limiting
// ---------------------------------------------------------------------------

// Its own bucket, and therefore its own test: the auth limiter is left wide open here, so a
// refusal can only be coming from the one that counts messages.
TEST(AuthEndpointTest, ThrottlesTheRoutesThatSendAMessage) {
    LAUNCHER_REQUIRE_DATABASE();

    constexpr std::size_t ATTEMPTS = 3;
    const ScopedMailRateLimit limit(ATTEMPTS, std::chrono::seconds{60});

    Json::Value request;
    request["email"] = "never-registered@example.test";

    for (std::size_t sent = 0; sent < ATTEMPTS; ++sent) {
        ASSERT_EQ(harness().postJson("/api/v1/auth/verify-email/resend", request)->statusCode(),
                  drogon::k200OK)
            << "attempt " << sent;
    }

    const auto limited = harness().postJson("/api/v1/auth/verify-email/resend", request);
    ASSERT_EQ(limited->statusCode(), drogon::k429TooManyRequests);
    EXPECT_EQ(bodyOf(limited)["code"].asString(), "rate_limited");
    EXPECT_FALSE(limited->getHeader("Retry-After").empty());

    // The same bucket, shared with the reset request: they spend one allowance between them,
    // because they cost the same thing.
    Json::Value reset;
    reset["email"] = "never-registered@example.test";
    EXPECT_EQ(harness().postJson("/api/v1/auth/password-reset/request", reset)->statusCode(),
              drogon::k429TooManyRequests);
}

// Proves the filter is actually attached to the route; the algorithm itself is covered
// deterministically by RateLimiterTest.
TEST(AuthEndpointTest, ThrottlesRepeatedLoginAttempts) {
    LAUNCHER_REQUIRE_DATABASE();

    // Both constructing and destroying this empties the bucket, so the count below is exact
    // and nothing is left behind for the next test.
    constexpr std::size_t ATTEMPTS = 5;
    const ScopedAuthRateLimit limit(ATTEMPTS, std::chrono::seconds{60});

    const auto attempt = []() {
        return harness().postJson("/api/v1/auth/login",
                                  credentials("nobody@example.test", "wrong password here"));
    };

    for (std::size_t sent = 0; sent < ATTEMPTS; ++sent) {
        ASSERT_EQ(attempt()->statusCode(), drogon::k401Unauthorized) << "attempt " << sent;
    }

    const auto limited = attempt();
    ASSERT_EQ(limited->statusCode(), drogon::k429TooManyRequests);
    EXPECT_EQ(bodyOf(limited)["code"].asString(), "rate_limited");
    EXPECT_FALSE(limited->getHeader("Retry-After").empty());
}

} // namespace
