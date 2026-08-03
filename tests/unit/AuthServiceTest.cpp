#include <gtest/gtest.h>

#include <drogon/utils/coroutine.h>

#include <memory>
#include <string>

#include "common/Hash.h"
#include "services/AuthService.h"
#include "support/FakeRepositories.h"

namespace {

using launcher::common::ErrorCode;
using launcher::domain::User;
using launcher::repositories::UserTokenPurpose;
using launcher::services::Argon2idPasswordHasher;
using launcher::services::AuthService;
using launcher::services::AuthSettings;
using launcher::services::ClientContext;
using launcher::services::JwtTokenService;
using launcher::services::PasswordHashingSettings;
using launcher::services::RegisterCommand;
using launcher::services::TokenSettings;
using launcher::testing::FakeRefreshTokenRepository;
using launcher::testing::FakeRoleRepository;
using launcher::testing::FakeUserRepository;
using launcher::testing::FakeUserTokenRepository;

constexpr const char* VALID_PASSWORD = "correct horse battery staple";
constexpr const char* JWT_SECRET = "0123456789abcdef0123456789abcdef";

/// Wires a service over fresh fakes. Everything is kept alive by the fixture so the
/// service's references stay valid for the whole test.
struct AuthFixture {
    explicit AuthFixture(AuthSettings settings = AuthSettings{})
        : hasher(PasswordHashingSettings{2, 64U * 1024U * 1024U}),
          tokens(TokenSettings{JWT_SECRET, "custom-game-launcher", std::chrono::seconds{900}}),
          service(users, roles, refreshTokens, userTokens, hasher, tokens, std::move(settings)) {}

    FakeUserRepository users;
    FakeRoleRepository roles;
    FakeRefreshTokenRepository refreshTokens;
    FakeUserTokenRepository userTokens;
    Argon2idPasswordHasher hasher;
    JwtTokenService tokens;
    AuthService service;

    /// Seeds a ready-to-log-in account with a real hash of VALID_PASSWORD.
    User seedActiveUser(const std::string& email = "dev@example.com") {
        User user;
        user.email = email;
        user.displayName = "Dev";
        user.passwordHash = hasher.hash(VALID_PASSWORD).value();
        user.emailVerified = true;
        user.isActive = true;
        return users.seed(user);
    }
};

AuthSettings withoutEmailVerification() {
    AuthSettings settings;
    settings.requireVerifiedEmail = false;
    return settings;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

TEST(AuthServiceRegisterTest, CreatesTheAccountAndGrantsTheDefaultRole) {
    AuthFixture fixture;

    const auto result = drogon::sync_wait(
        fixture.service.registerUser(RegisterCommand{"Dev@Example.COM", VALID_PASSWORD, "Dev"}));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_EQ(result.value().user.email, "dev@example.com") << "the email should be normalised";
    EXPECT_EQ(result.value().user.displayName, "Dev");
    ASSERT_EQ(fixture.roles.assignments.size(), 1U);
    EXPECT_EQ(fixture.roles.assignments[0].second, "player");
}

TEST(AuthServiceRegisterTest, NeverStoresThePasswordInPlaintext) {
    AuthFixture fixture;

    const auto result = drogon::sync_wait(
        fixture.service.registerUser(RegisterCommand{"dev@example.com", VALID_PASSWORD, "Dev"}));

    ASSERT_TRUE(result.ok());
    const auto& stored = result.value().user.passwordHash;
    EXPECT_EQ(stored.find(VALID_PASSWORD), std::string::npos);
    EXPECT_EQ(stored.rfind("$argon2id$", 0), 0U);
}

TEST(AuthServiceRegisterTest, IssuesAnEmailVerificationTokenStoredOnlyAsAHash) {
    AuthFixture fixture;

    const auto result = drogon::sync_wait(
        fixture.service.registerUser(RegisterCommand{"dev@example.com", VALID_PASSWORD, "Dev"}));

    ASSERT_TRUE(result.ok());
    const auto& raw = result.value().emailVerificationToken;
    ASSERT_FALSE(raw.empty());
    ASSERT_EQ(fixture.userTokens.entries.size(), 1U);
    EXPECT_EQ(fixture.userTokens.entries[0].purpose, UserTokenPurpose::EmailVerification);
    EXPECT_EQ(fixture.userTokens.entries[0].tokenHash, launcher::common::sha256Hex(raw));
    EXPECT_NE(fixture.userTokens.entries[0].tokenHash, raw);
}

TEST(AuthServiceRegisterTest, RejectsInvalidInput) {
    AuthFixture fixture;

    const auto badEmail = drogon::sync_wait(
        fixture.service.registerUser(RegisterCommand{"not-an-email", VALID_PASSWORD, "Dev"}));
    const auto shortPassword = drogon::sync_wait(
        fixture.service.registerUser(RegisterCommand{"dev@example.com", "short", "Dev"}));
    const auto shortName = drogon::sync_wait(
        fixture.service.registerUser(RegisterCommand{"dev@example.com", VALID_PASSWORD, "D"}));

    EXPECT_EQ(badEmail.error().code, ErrorCode::InvalidInput);
    EXPECT_EQ(shortPassword.error().code, ErrorCode::InvalidInput);
    EXPECT_EQ(shortName.error().code, ErrorCode::InvalidInput);
    EXPECT_TRUE(fixture.users.users.empty());
}

TEST(AuthServiceRegisterTest, ReportsAConflictForAnAlreadyRegisteredAddress) {
    AuthFixture fixture;
    fixture.seedActiveUser("dev@example.com");

    const auto result = drogon::sync_wait(
        fixture.service.registerUser(RegisterCommand{"DEV@example.com", VALID_PASSWORD, "Dev"}));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::Conflict);
}

// ---------------------------------------------------------------------------
// Login
// ---------------------------------------------------------------------------

TEST(AuthServiceLoginTest, IssuesTokensForValidCredentials) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();
    fixture.roles.permissionsByUser[user.id] = {"library.read", "game.download"};

    const auto result =
        drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, ClientContext{}));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_FALSE(result.value().accessToken.empty());
    EXPECT_FALSE(result.value().refreshToken.empty());
    EXPECT_EQ(result.value().permissions,
              std::vector<std::string>({"library.read", "game.download"}));
    EXPECT_EQ(fixture.users.loginRecordedFor, std::vector<std::string>({user.id}));
}

// The database must never hold anything that can be replayed as a session.
TEST(AuthServiceLoginTest, StoresOnlyTheHashOfTheRefreshToken) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();

    const auto result =
        drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, ClientContext{}));

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(fixture.refreshTokens.entries.size(), 1U);
    const auto& stored = fixture.refreshTokens.entries[0].tokenHash;
    EXPECT_EQ(stored, launcher::common::sha256Hex(result.value().refreshToken));
    EXPECT_NE(stored, result.value().refreshToken);
}

TEST(AuthServiceLoginTest, IssuesAnAccessTokenCarryingTheUsersPermissions) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();
    fixture.roles.permissionsByUser[user.id] = {"build.upload"};

    const auto result =
        drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, ClientContext{}));
    ASSERT_TRUE(result.ok());

    const auto claims = fixture.tokens.verifyAccessToken(result.value().accessToken);
    ASSERT_TRUE(claims.ok());
    EXPECT_EQ(claims.value().userId, user.id);
    EXPECT_TRUE(claims.value().hasPermission("build.upload"));
    EXPECT_FALSE(claims.value().hasPermission("admin.users.manage"));
}

// Different messages here would turn login into a registered-address oracle.
TEST(AuthServiceLoginTest, GivesTheSameAnswerForAnUnknownEmailAndAWrongPassword) {
    AuthFixture fixture;
    fixture.seedActiveUser("dev@example.com");

    const auto unknown = drogon::sync_wait(
        fixture.service.login("nobody@example.com", VALID_PASSWORD, ClientContext{}));
    const auto wrong = drogon::sync_wait(
        fixture.service.login("dev@example.com", "wrong password entirely", ClientContext{}));

    ASSERT_FALSE(unknown.ok());
    ASSERT_FALSE(wrong.ok());
    EXPECT_EQ(unknown.error().code, ErrorCode::Unauthenticated);
    EXPECT_EQ(wrong.error().code, ErrorCode::Unauthenticated);
    EXPECT_EQ(unknown.error().detail, wrong.error().detail);
}

TEST(AuthServiceLoginTest, RefusesADisabledAccount) {
    AuthFixture fixture;
    auto user = fixture.seedActiveUser();
    fixture.users.users[0].isActive = false;

    const auto result =
        drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, ClientContext{}));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::Forbidden);
    EXPECT_TRUE(fixture.refreshTokens.entries.empty());
}

TEST(AuthServiceLoginTest, RefusesAnUnverifiedAccountWhenVerificationIsRequired) {
    AuthFixture fixture;
    auto user = fixture.seedActiveUser();
    fixture.users.users[0].emailVerified = false;

    const auto result =
        drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, ClientContext{}));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::Forbidden);
}

TEST(AuthServiceLoginTest, AllowsAnUnverifiedAccountWhenVerificationIsOptional) {
    AuthFixture fixture(withoutEmailVerification());
    auto user = fixture.seedActiveUser();
    fixture.users.users[0].emailVerified = false;

    const auto result =
        drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, ClientContext{}));

    EXPECT_TRUE(result.ok()) << result.error().detail;
}

TEST(AuthServiceLoginTest, MatchesTheEmailCaseInsensitively) {
    AuthFixture fixture;
    fixture.seedActiveUser("dev@example.com");

    const auto result = drogon::sync_wait(
        fixture.service.login("  DEV@Example.Com ", VALID_PASSWORD, ClientContext{}));

    EXPECT_TRUE(result.ok()) << result.error().detail;
}

// Raising the Argon2id cost must upgrade stored hashes silently on the next login.
TEST(AuthServiceLoginTest, UpgradesAHashProducedWithWeakerParameters) {
    AuthFixture fixture;
    const Argon2idPasswordHasher weak(PasswordHashingSettings{1, 8U * 1024U * 1024U});
    User user;
    user.email = "dev@example.com";
    user.displayName = "Dev";
    user.passwordHash = weak.hash(VALID_PASSWORD).value();
    user.emailVerified = true;
    const auto seeded = fixture.users.seed(user);

    const auto result =
        drogon::sync_wait(fixture.service.login(seeded.email, VALID_PASSWORD, ClientContext{}));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    ASSERT_EQ(fixture.users.passwordUpdates.count(seeded.id), 1U);
    EXPECT_NE(fixture.users.passwordUpdates[seeded.id], user.passwordHash);
}

// ---------------------------------------------------------------------------
// Refresh rotation
// ---------------------------------------------------------------------------

TEST(AuthServiceRefreshTest, RotatesWithinTheSameFamily) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();
    const auto session =
        drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, ClientContext{}));
    ASSERT_TRUE(session.ok());
    const auto originalFamily = fixture.refreshTokens.entries[0].record.familyId;

    const auto refreshed =
        drogon::sync_wait(fixture.service.refresh(session.value().refreshToken, ClientContext{}));

    ASSERT_TRUE(refreshed.ok()) << refreshed.error().detail;
    EXPECT_NE(refreshed.value().refreshToken, session.value().refreshToken);
    ASSERT_EQ(fixture.refreshTokens.rotations.size(), 1U);
    ASSERT_EQ(fixture.refreshTokens.entries.size(), 2U);
    EXPECT_EQ(fixture.refreshTokens.entries[1].record.familyId, originalFamily);
    EXPECT_TRUE(fixture.refreshTokens.entries[0].record.revoked)
        << "the presented token must not stay usable";
}

TEST(AuthServiceRefreshTest, RejectsAnUnknownToken) {
    AuthFixture fixture;

    const auto result = drogon::sync_wait(fixture.service.refresh("never-issued", ClientContext{}));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::Unauthenticated);
}

// The central anti-theft rule: replaying a rotated token burns the whole family.
TEST(AuthServiceRefreshTest, ReplayingARotatedTokenRevokesTheEntireFamily) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();
    const auto session =
        drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, ClientContext{}));
    ASSERT_TRUE(session.ok());
    const auto family = fixture.refreshTokens.entries[0].record.familyId;

    const auto firstRefresh =
        drogon::sync_wait(fixture.service.refresh(session.value().refreshToken, ClientContext{}));
    ASSERT_TRUE(firstRefresh.ok());

    // The attacker (or a confused client) replays the token that was already rotated.
    const auto replay =
        drogon::sync_wait(fixture.service.refresh(session.value().refreshToken, ClientContext{}));

    ASSERT_FALSE(replay.ok());
    EXPECT_EQ(replay.error().code, ErrorCode::Unauthenticated);
    EXPECT_EQ(fixture.refreshTokens.revokedFamilies, std::vector<std::string>({family}));

    // The token handed out by the legitimate refresh is dead too.
    const auto afterBurn = drogon::sync_wait(
        fixture.service.refresh(firstRefresh.value().refreshToken, ClientContext{}));
    EXPECT_FALSE(afterBurn.ok());
}

TEST(AuthServiceRefreshTest, RejectsAndRevokesAnExpiredToken) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();
    const auto session =
        drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, ClientContext{}));
    ASSERT_TRUE(session.ok());
    fixture.refreshTokens.entries[0].record.expired = true;

    const auto result =
        drogon::sync_wait(fixture.service.refresh(session.value().refreshToken, ClientContext{}));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::Unauthenticated);
    EXPECT_EQ(fixture.refreshTokens.revokedIds.size(), 1U);
}

TEST(AuthServiceRefreshTest, RefusesAndClearsSessionsForADisabledAccount) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();
    const auto session =
        drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, ClientContext{}));
    ASSERT_TRUE(session.ok());
    fixture.users.users[0].isActive = false;

    const auto result =
        drogon::sync_wait(fixture.service.refresh(session.value().refreshToken, ClientContext{}));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::Forbidden);
    EXPECT_EQ(fixture.refreshTokens.revokedUsers, std::vector<std::string>({user.id}));
}

// ---------------------------------------------------------------------------
// Logout
// ---------------------------------------------------------------------------

TEST(AuthServiceLogoutTest, RevokesThePresentedToken) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();
    const auto session =
        drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, ClientContext{}));
    ASSERT_TRUE(session.ok());

    const auto result = drogon::sync_wait(fixture.service.logout(session.value().refreshToken));

    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(fixture.refreshTokens.entries[0].record.revoked);
}

// Logout must not become a way to test whether a token exists.
TEST(AuthServiceLogoutTest, SucceedsForAnUnknownToken) {
    AuthFixture fixture;

    EXPECT_TRUE(drogon::sync_wait(fixture.service.logout("never-issued")).ok());
}

// ---------------------------------------------------------------------------
// Email verification
// ---------------------------------------------------------------------------

TEST(AuthServiceVerifyEmailTest, MarksTheAccountVerified) {
    AuthFixture fixture;
    const auto registration = drogon::sync_wait(
        fixture.service.registerUser(RegisterCommand{"dev@example.com", VALID_PASSWORD, "Dev"}));
    ASSERT_TRUE(registration.ok());

    const auto result =
        drogon::sync_wait(fixture.service.verifyEmail(registration.value().emailVerificationToken));

    EXPECT_TRUE(result.ok()) << result.error().detail;
    EXPECT_EQ(fixture.users.verifiedUserIds,
              std::vector<std::string>({registration.value().user.id}));
}

TEST(AuthServiceVerifyEmailTest, ATokenCanOnlyBeUsedOnce) {
    AuthFixture fixture;
    const auto registration = drogon::sync_wait(
        fixture.service.registerUser(RegisterCommand{"dev@example.com", VALID_PASSWORD, "Dev"}));
    ASSERT_TRUE(registration.ok());
    const auto token = registration.value().emailVerificationToken;

    ASSERT_TRUE(drogon::sync_wait(fixture.service.verifyEmail(token)).ok());

    EXPECT_FALSE(drogon::sync_wait(fixture.service.verifyEmail(token)).ok());
}

TEST(AuthServiceVerifyEmailTest, RejectsAnUnknownToken) {
    AuthFixture fixture;

    const auto result = drogon::sync_wait(fixture.service.verifyEmail("made-up"));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
}

// ---------------------------------------------------------------------------
// Password reset
// ---------------------------------------------------------------------------

// An unauthenticated endpoint that says "no such user" is an enumeration tool.
TEST(AuthServicePasswordResetTest, ReportsSuccessWithNoTokenForAnUnknownAddress) {
    AuthFixture fixture;

    const auto result =
        drogon::sync_wait(fixture.service.requestPasswordReset("nobody@example.com"));

    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value().token.has_value());
    EXPECT_TRUE(fixture.userTokens.entries.empty());
}

TEST(AuthServicePasswordResetTest, IssuesAHashedSingleUseTokenForAKnownAddress) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();

    const auto result = drogon::sync_wait(fixture.service.requestPasswordReset(user.email));

    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.value().token.has_value());
    ASSERT_EQ(fixture.userTokens.entries.size(), 1U);
    EXPECT_EQ(fixture.userTokens.entries[0].tokenHash,
              launcher::common::sha256Hex(*result.value().token));
}

TEST(AuthServicePasswordResetTest, RequestingANewLinkInvalidatesTheOldOne) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();

    const auto first = drogon::sync_wait(fixture.service.requestPasswordReset(user.email));
    const auto second = drogon::sync_wait(fixture.service.requestPasswordReset(user.email));
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());

    EXPECT_FALSE(drogon::sync_wait(
                     fixture.service.resetPassword(*first.value().token, "a brand new passphrase"))
                     .ok());
    EXPECT_TRUE(drogon::sync_wait(
                    fixture.service.resetPassword(*second.value().token, "a brand new passphrase"))
                    .ok());
}

TEST(AuthServicePasswordResetTest, ChangesThePasswordAndKillsEverySession) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();
    ASSERT_TRUE(
        drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, ClientContext{})).ok());
    const auto request = drogon::sync_wait(fixture.service.requestPasswordReset(user.email));
    ASSERT_TRUE(request.ok());

    const auto result = drogon::sync_wait(
        fixture.service.resetPassword(*request.value().token, "an entirely new passphrase"));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    ASSERT_EQ(fixture.users.passwordUpdates.count(user.id), 1U);
    // A reset is the recovery path after a compromise, so old sessions must not survive it.
    EXPECT_EQ(fixture.refreshTokens.revokedUsers, std::vector<std::string>({user.id}));

    EXPECT_FALSE(
        drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, ClientContext{})).ok());
    EXPECT_TRUE(drogon::sync_wait(fixture.service.login(
                                      user.email, "an entirely new passphrase", ClientContext{}))
                    .ok());
}

TEST(AuthServicePasswordResetTest, RejectsAWeakNewPasswordBeforeConsumingTheToken) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();
    const auto request = drogon::sync_wait(fixture.service.requestPasswordReset(user.email));
    ASSERT_TRUE(request.ok());

    const auto weak =
        drogon::sync_wait(fixture.service.resetPassword(*request.value().token, "short"));

    ASSERT_FALSE(weak.ok());
    EXPECT_EQ(weak.error().code, ErrorCode::InvalidInput);
    // The link must survive a rejected attempt, or a typo would strand the user.
    EXPECT_TRUE(drogon::sync_wait(
                    fixture.service.resetPassword(*request.value().token, "a brand new passphrase"))
                    .ok());
}

TEST(AuthServicePasswordResetTest, RejectsAnUnknownToken) {
    AuthFixture fixture;

    const auto result =
        drogon::sync_wait(fixture.service.resetPassword("made-up", "a brand new passphrase"));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
}

} // namespace
