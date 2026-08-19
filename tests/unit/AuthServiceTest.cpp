#include <gtest/gtest.h>

#include <drogon/utils/coroutine.h>

#include <memory>
#include <string>

#include <algorithm>

#include "common/Hash.h"
#include "common/Random.h"
#include "domain/ValidationRules.h"
#include "services/AuthService.h"
#include "support/FakeMailSender.h"
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
using launcher::testing::FakeMailSender;
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
          service(
              users, roles, refreshTokens, userTokens, hasher, tokens, mail, std::move(settings)) {}

    FakeUserRepository users;
    FakeRoleRepository roles;
    FakeRefreshTokenRepository refreshTokens;
    FakeUserTokenRepository userTokens;
    Argon2idPasswordHasher hasher;
    JwtTokenService tokens;
    FakeMailSender mail;
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
    // Read where a recipient reads it: the raw token exists for the length of one function and
    // is never returned to anybody, so the message is the only place it can be found.
    const auto raw = fixture.mail.tokenIn("dev@example.com");
    ASSERT_FALSE(raw.empty());
    ASSERT_EQ(fixture.userTokens.entries.size(), 1U);
    EXPECT_EQ(fixture.userTokens.entries[0].purpose, UserTokenPurpose::EmailVerification);
    EXPECT_EQ(fixture.userTokens.entries[0].tokenHash, launcher::common::sha256Hex(raw));
    EXPECT_NE(fixture.userTokens.entries[0].tokenHash, raw);
}

TEST(AuthServiceRegisterTest, SendsTheVerificationLinkAndSaysThatItWent) {
    AuthFixture fixture;

    const auto result = drogon::sync_wait(
        fixture.service.registerUser(RegisterCommand{"dev@example.com", VALID_PASSWORD, "Dev"}));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_TRUE(result.value().verificationEmailSent);
    ASSERT_EQ(fixture.mail.sentCount(), 1U);
    const auto message = fixture.mail.lastTo("dev@example.com");
    ASSERT_TRUE(message.has_value());
    EXPECT_NE(message->body.find("/verify-email?token="), std::string::npos)
        << "the message must carry the link a browser can open, not a bare token";
}

// The failure this whole feature exists to make survivable: the account is created, the
// message is not, and the caller is told which of the two happened. Undoing the registration
// would mean a compensating delete that can fail on its own — and when it does, the address is
// taken by an account that cannot sign in and cannot be created again.
TEST(AuthServiceRegisterTest, KeepsTheAccountWhenTheMessageCannotBeSent) {
    AuthFixture fixture;
    fixture.mail.refuseEverything(true);

    const auto result = drogon::sync_wait(
        fixture.service.registerUser(RegisterCommand{"dev@example.com", VALID_PASSWORD, "Dev"}));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_FALSE(result.value().verificationEmailSent);
    ASSERT_EQ(fixture.users.users.size(), 1U) << "the account must survive a relay that was down";
    EXPECT_EQ(fixture.users.users[0].email, "dev@example.com");
    EXPECT_EQ(fixture.roles.assignments.size(), 1U);
    // The token is issued whether or not the message went out, so the resend route has
    // something to replace and the account is in one state rather than two.
    ASSERT_EQ(fixture.userTokens.entries.size(), 1U);
    EXPECT_EQ(fixture.userTokens.entries[0].purpose, UserTokenPurpose::EmailVerification);
}

TEST(AuthServiceRegisterTest, ComposesNothingWhenTheDeploymentSendsNoMail) {
    AuthSettings settings = withoutEmailVerification();
    settings.mailEnabled = false;
    AuthFixture fixture(settings);

    const auto result = drogon::sync_wait(
        fixture.service.registerUser(RegisterCommand{"dev@example.com", VALID_PASSWORD, "Dev"}));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_FALSE(result.value().verificationEmailSent);
    EXPECT_EQ(fixture.mail.sentCount(), 0U);
    EXPECT_EQ(fixture.mail.refusedCount(), 0U) << "nothing should have been handed to the sender";
    EXPECT_TRUE(fixture.userTokens.entries.empty())
        << "a link nothing can deliver is not worth issuing";
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
    ASSERT_EQ(fixture.users.rehashes.count(seeded.id), 1U);
    EXPECT_NE(fixture.users.rehashes[seeded.id], user.passwordHash);

    // Through rehashPassword and not updatePasswordHash, which clears the forced change:
    // nobody chose a password here, and signing in once with an operator's temporary one must
    // not be enough to keep it.
    EXPECT_EQ(fixture.users.passwordUpdates.count(seeded.id), 0U);
}

// The other half of the same rule, stated as its own failure: an account holding a one-time
// password whose hash happens to want upgrading is still an account that has to change it.
TEST(AuthServiceLoginTest, AnUpgradedHashDoesNotClearTheForcedPasswordChange) {
    AuthFixture fixture;
    const Argon2idPasswordHasher weak(PasswordHashingSettings{1, 8U * 1024U * 1024U});
    User user;
    user.email = "dev@example.com";
    user.displayName = "Dev";
    user.passwordHash = weak.hash(VALID_PASSWORD).value();
    user.emailVerified = true;
    user.passwordChangeRequired = true;
    fixture.users.seed(user);

    const auto result =
        drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, ClientContext{}));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_TRUE(fixture.users.users[0].passwordChangeRequired);

    const auto claims = fixture.tokens.verifyAccessToken(result.value().accessToken);
    ASSERT_TRUE(claims.ok());
    EXPECT_TRUE(claims.value().passwordChangeRequired);
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
        drogon::sync_wait(fixture.service.verifyEmail(fixture.mail.tokenIn("dev@example.com")));

    EXPECT_TRUE(result.ok()) << result.error().detail;
    EXPECT_EQ(fixture.users.verifiedUserIds,
              std::vector<std::string>({registration.value().user.id}));
}

TEST(AuthServiceVerifyEmailTest, ATokenCanOnlyBeUsedOnce) {
    AuthFixture fixture;
    const auto registration = drogon::sync_wait(
        fixture.service.registerUser(RegisterCommand{"dev@example.com", VALID_PASSWORD, "Dev"}));
    ASSERT_TRUE(registration.ok());
    const auto token = fixture.mail.tokenIn("dev@example.com");

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
// Resending the verification link
// ---------------------------------------------------------------------------

TEST(AuthServiceResendTest, SendsAFreshLinkAndRetiresTheOldOne) {
    AuthFixture fixture;
    ASSERT_TRUE(drogon::sync_wait(fixture.service.registerUser(
                                      RegisterCommand{"dev@example.com", VALID_PASSWORD, "Dev"}))
                    .ok());
    const auto first = fixture.mail.tokenIn("dev@example.com");

    const auto result = drogon::sync_wait(fixture.service.resendVerification("Dev@Example.COM"));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    const auto second = fixture.mail.tokenIn("dev@example.com");
    ASSERT_FALSE(second.empty());
    ASSERT_NE(first, second);
    // Only the newest link works: a resend that left the previous one alive would mean a
    // message somebody asked to replace still opens.
    EXPECT_FALSE(drogon::sync_wait(fixture.service.verifyEmail(first)).ok());
    EXPECT_TRUE(drogon::sync_wait(fixture.service.verifyEmail(second)).ok());
}

// The same sentence for an address nobody registered, one already confirmed and a disabled
// account — and nothing sent in any of the three, which is the part a caller could time.
TEST(AuthServiceResendTest, SendsNothingForAnUnknownAddress) {
    AuthFixture fixture;

    const auto result = drogon::sync_wait(fixture.service.resendVerification("nobody@example.com"));

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(fixture.mail.sentCount(), 0U);
    EXPECT_TRUE(fixture.userTokens.entries.empty());
}

TEST(AuthServiceResendTest, SendsNothingForAnAddressThatIsAlreadyConfirmed) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();

    const auto result = drogon::sync_wait(fixture.service.resendVerification(user.email));

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(fixture.mail.sentCount(), 0U);
}

TEST(AuthServiceResendTest, SendsNothingToADisabledAccount) {
    AuthFixture fixture;
    User disabled;
    disabled.email = "gone@example.com";
    disabled.displayName = "Gone";
    disabled.emailVerified = false;
    disabled.isActive = false;
    fixture.users.seed(disabled);

    const auto result = drogon::sync_wait(fixture.service.resendVerification(disabled.email));

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(fixture.mail.sentCount(), 0U);
}

// ---------------------------------------------------------------------------
// Password reset
// ---------------------------------------------------------------------------

// An unauthenticated endpoint that says "no such user" is an enumeration tool. The answer is
// the same either way *and* nothing is sent, which is the half a caller could otherwise time.
TEST(AuthServicePasswordResetTest, ReportsSuccessAndSendsNothingForAnUnknownAddress) {
    AuthFixture fixture;

    const auto result =
        drogon::sync_wait(fixture.service.requestPasswordReset("nobody@example.com"));

    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(fixture.userTokens.entries.empty());
    EXPECT_EQ(fixture.mail.sentCount(), 0U);
}

TEST(AuthServicePasswordResetTest, IssuesAHashedSingleUseTokenAndSendsItToTheAccount) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();

    const auto result = drogon::sync_wait(fixture.service.requestPasswordReset(user.email));

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(fixture.userTokens.entries.size(), 1U);
    const auto delivered = fixture.mail.tokenIn(user.email);
    ASSERT_FALSE(delivered.empty());
    EXPECT_EQ(fixture.userTokens.entries[0].tokenHash, launcher::common::sha256Hex(delivered));
}

// The reply cannot say the message failed without saying the address exists, so the caller is
// told the same thing and the failure is a log line. What matters is that nothing else changes.
TEST(AuthServicePasswordResetTest, StillReportsSuccessWhenTheMessageCannotBeSent) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();
    fixture.mail.refuseEverything(true);

    const auto result = drogon::sync_wait(fixture.service.requestPasswordReset(user.email));

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(fixture.mail.refusedCount(), 1U);
    EXPECT_EQ(fixture.mail.sentCount(), 0U);
}

TEST(AuthServicePasswordResetTest, RequestingANewLinkInvalidatesTheOldOne) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();

    ASSERT_TRUE(drogon::sync_wait(fixture.service.requestPasswordReset(user.email)).ok());
    const auto first = fixture.mail.tokenIn(user.email);
    ASSERT_TRUE(drogon::sync_wait(fixture.service.requestPasswordReset(user.email)).ok());
    const auto second = fixture.mail.tokenIn(user.email);
    ASSERT_NE(first, second);

    EXPECT_FALSE(
        drogon::sync_wait(fixture.service.resetPassword(first, "a brand new passphrase")).ok());
    EXPECT_TRUE(
        drogon::sync_wait(fixture.service.resetPassword(second, "a brand new passphrase")).ok());
}

TEST(AuthServicePasswordResetTest, ChangesThePasswordAndKillsEverySession) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();
    ASSERT_TRUE(
        drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, ClientContext{})).ok());
    ASSERT_TRUE(drogon::sync_wait(fixture.service.requestPasswordReset(user.email)).ok());

    const auto result = drogon::sync_wait(fixture.service.resetPassword(
        fixture.mail.tokenIn(user.email), "an entirely new passphrase"));

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
    ASSERT_TRUE(drogon::sync_wait(fixture.service.requestPasswordReset(user.email)).ok());
    const auto token = fixture.mail.tokenIn(user.email);

    const auto weak = drogon::sync_wait(fixture.service.resetPassword(token, "short"));

    ASSERT_FALSE(weak.ok());
    EXPECT_EQ(weak.error().code, ErrorCode::InvalidInput);
    // The link must survive a rejected attempt, or a typo would strand the user.
    EXPECT_TRUE(
        drogon::sync_wait(fixture.service.resetPassword(token, "a brand new passphrase")).ok());
}

// ---------------------------------------------------------------------------
// Changing the password from inside a session — the way out of a one-time password
// ---------------------------------------------------------------------------

TEST(AuthServiceChangePasswordTest, ReplacesThePasswordAndHandsBackAWorkingSession) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();

    const auto changed = drogon::sync_wait(
        fixture.service.changePassword(user.id, VALID_PASSWORD, "a brand new passphrase", {}));

    ASSERT_TRUE(changed.ok()) << changed.error().detail;
    EXPECT_FALSE(changed.value().accessToken.empty());
    EXPECT_FALSE(changed.value().refreshToken.empty());

    // The old one is gone and the new one works, which is the whole of what "changed" means.
    EXPECT_FALSE(drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, {})).ok());
    EXPECT_TRUE(
        drogon::sync_wait(fixture.service.login(user.email, "a brand new passphrase", {})).ok());
}

// The flag is what refuses every route, and the session handed back here is the one the
// caller carries on with — minting it from a stale copy would leave them still locked out
// after doing exactly what they were told to do.
TEST(AuthServiceChangePasswordTest, TheNewSessionNoLongerCarriesTheForcedChange) {
    AuthFixture fixture;
    auto user = fixture.seedActiveUser();
    fixture.users.users[0].passwordChangeRequired = true;

    const auto changed = drogon::sync_wait(
        fixture.service.changePassword(user.id, VALID_PASSWORD, "a brand new passphrase", {}));

    ASSERT_TRUE(changed.ok()) << changed.error().detail;
    EXPECT_FALSE(fixture.users.users[0].passwordChangeRequired);

    const auto claims = fixture.tokens.verifyAccessToken(changed.value().accessToken);
    ASSERT_TRUE(claims.ok());
    EXPECT_FALSE(claims.value().passwordChangeRequired);
}

// D44's rule on the other request that can take an account from its owner: a valid token says
// who is asking, not that the owner is the one at the keyboard.
TEST(AuthServiceChangePasswordTest, AsksForTheCurrentPasswordAgain) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();

    const auto refused = drogon::sync_wait(
        fixture.service.changePassword(user.id, "not it", "a brand new passphrase", {}));

    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, ErrorCode::Unauthenticated);
    EXPECT_TRUE(fixture.hasher.verify(VALID_PASSWORD, fixture.users.users[0].passwordHash));
}

// Elsewhere this would be a harmless no-op. Here it is the feature defeated: re-entering the
// operator's temporary password would clear the flag and leave the account on a credential
// somebody else knows.
TEST(AuthServiceChangePasswordTest, TheNewPasswordCannotBeTheOldOne) {
    AuthFixture fixture;
    auto user = fixture.seedActiveUser();
    fixture.users.users[0].passwordChangeRequired = true;

    const auto refused = drogon::sync_wait(
        fixture.service.changePassword(user.id, VALID_PASSWORD, VALID_PASSWORD, {}));

    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidInput);
    EXPECT_EQ(refused.error().rule, launcher::domain::rules::PASSWORD_UNCHANGED);
    EXPECT_TRUE(fixture.users.users[0].passwordChangeRequired);
}

TEST(AuthServiceChangePasswordTest, TheNewPasswordStillHasToPassThePolicy) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();

    const auto refused =
        drogon::sync_wait(fixture.service.changePassword(user.id, VALID_PASSWORD, "short", {}));

    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidInput);
    // Checked before the current password is even verified, so a weak new password costs no
    // Argon2id verification — and the account is untouched either way.
    EXPECT_TRUE(fixture.hasher.verify(VALID_PASSWORD, fixture.users.users[0].passwordHash));
}

// A password change is what somebody does after a credential leaked, so the sessions minted
// under the old one cannot outlive it.
TEST(AuthServiceChangePasswordTest, EveryOtherSessionAndEveryResetLinkDies) {
    AuthFixture fixture;
    const auto user = fixture.seedActiveUser();
    ASSERT_TRUE(drogon::sync_wait(fixture.service.login(user.email, VALID_PASSWORD, {})).ok());

    ASSERT_TRUE(drogon::sync_wait(fixture.service.changePassword(
                                      user.id, VALID_PASSWORD, "a brand new passphrase", {}))
                    .ok());

    EXPECT_NE(std::find(fixture.refreshTokens.revokedUsers.begin(),
                        fixture.refreshTokens.revokedUsers.end(),
                        user.id),
              fixture.refreshTokens.revokedUsers.end());
    EXPECT_FALSE(fixture.userTokens.invalidations.empty());
}

TEST(AuthServiceChangePasswordTest, RejectsAnAccountThatIsNoLongerThere) {
    AuthFixture fixture;

    const auto refused = drogon::sync_wait(fixture.service.changePassword(
        launcher::common::randomUuid(), VALID_PASSWORD, "a brand new passphrase", {}));

    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, ErrorCode::Unauthenticated);
}

TEST(AuthServicePasswordResetTest, RejectsAnUnknownToken) {
    AuthFixture fixture;

    const auto result =
        drogon::sync_wait(fixture.service.resetPassword("made-up", "a brand new passphrase"));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
}

} // namespace
