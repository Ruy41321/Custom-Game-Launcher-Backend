#include "services/AuthService.h"

#include <spdlog/spdlog.h>

#include <utility>

#include "common/Hash.h"
#include "common/Logging.h"
#include "common/Random.h"
#include "domain/Validation.h"

namespace launcher::services {
namespace {

using common::ErrorCode;
using common::Result;
using common::VoidResult;
using repositories::UserTokenPurpose;

/// One message for "no such account" and for "wrong password" alike. Distinguishing them
/// turns the login endpoint into a registered-address oracle.
constexpr const char* INVALID_CREDENTIALS = "email or password is incorrect";

constexpr const char* INVALID_REFRESH_TOKEN = "the refresh token is missing, expired or invalid";

} // namespace

AuthService::AuthService(const repositories::IUserRepository& users,
                         const repositories::IRoleRepository& roles,
                         const repositories::IRefreshTokenRepository& refreshTokens,
                         const repositories::IUserTokenRepository& userTokens,
                         const IPasswordHasher& passwordHasher,
                         const ITokenService& tokenService,
                         const IMailSender& mailSender,
                         AuthSettings settings)
    : users_(users),
      roles_(roles),
      refreshTokens_(refreshTokens),
      userTokens_(userTokens),
      passwordHasher_(passwordHasher),
      tokenService_(tokenService),
      mailSender_(mailSender),
      settings_(std::move(settings)) {}

drogon::Task<bool> AuthService::sendVerificationLink(domain::User user) const {
    if (!settings_.mailEnabled) {
        co_return false;
    }

    const auto token = common::randomUrlSafeToken();
    co_await userTokens_.issue(user.id,
                               UserTokenPurpose::EmailVerification,
                               common::sha256Hex(token),
                               settings_.emailVerificationTtl);

    auto message = verificationMessage(settings_.mail, user.email, user.displayName, token);
    const auto delivery = co_await mailSender_.send(std::move(message));
    if (!delivery.ok()) {
        // The account is not mentioned by address, and the token is never logged at all: this
        // line has to be readable by whoever operates the relay without being a way to read
        // somebody's mail out of the log.
        spdlog::error("could not send the verification message for user id={}: {}",
                      common::escapeJson(user.id),
                      common::escapeJson(delivery.error().detail));
    }
    co_return delivery.ok();
}

drogon::Task<Result<RegistrationResult>> AuthService::registerUser(RegisterCommand command) const {
    const auto email = domain::normalizeEmail(command.email);
    const auto displayName = domain::trim(command.displayName);

    if (auto check = domain::validateEmail(email); !check.ok()) {
        co_return Result<RegistrationResult>::failure(check.error());
    }
    if (auto check = domain::validateDisplayName(displayName); !check.ok()) {
        co_return Result<RegistrationResult>::failure(check.error());
    }
    if (auto check = domain::validatePassword(command.password); !check.ok()) {
        co_return Result<RegistrationResult>::failure(check.error());
    }

    auto passwordHash = passwordHasher_.hash(command.password);
    if (!passwordHash.ok()) {
        co_return Result<RegistrationResult>::failure(passwordHash.error());
    }

    // No "does this email exist" pre-check: the unique index decides, so two simultaneous
    // registrations of the same address cannot both succeed.
    auto created = co_await users_.create(
        domain::NewUser{email, displayName, std::move(passwordHash).value()});
    if (!created.ok()) {
        co_return Result<RegistrationResult>::failure(created.error());
    }

    const auto user = std::move(created).value();
    co_await roles_.assignRole(user.id, settings_.defaultRole, std::nullopt);

    // A message that does not go out does **not** undo the registration. Unwinding it would
    // mean a compensating delete that can fail on its own, and when it does the address is
    // taken by an account that cannot sign in and cannot be created again — an outage at the
    // relay turned into a lost account. The registration stands, the answer says the message
    // did not go, and the resend route is the way back.
    const bool sent = co_await sendVerificationLink(user);

    spdlog::info("registered user id={} role={} verificationEmailSent={}",
                 common::escapeJson(user.id),
                 common::escapeJson(settings_.defaultRole),
                 sent);

    co_return Result<RegistrationResult>::success(RegistrationResult{user, sent});
}

drogon::Task<VoidResult> AuthService::resendVerification(std::string email) const {
    const auto user = co_await users_.findByEmail(domain::normalizeEmail(email));

    // Three different reasons to send nothing, one answer for all of them and for success:
    // an address nobody registered, one that is already confirmed, and a disabled account.
    if (!user.has_value() || user->emailVerified || !user->isActive) {
        co_return VoidResult::success();
    }

    // Only the newest link works, exactly as for a password reset: a resend that left the
    // previous link alive would mean a message somebody asked to replace still opens.
    co_await userTokens_.invalidateAll(user->id, UserTokenPurpose::EmailVerification);
    co_await sendVerificationLink(*user);

    co_return VoidResult::success();
}

drogon::Task<Result<AuthTokens>>
AuthService::login(std::string email, std::string password, ClientContext client) const {
    const auto normalized = domain::normalizeEmail(email);
    const auto user = co_await users_.findByEmail(normalized);

    if (!user.has_value()) {
        // Spend the same time as a real verification would, so response latency does not
        // reveal whether the address is registered.
        passwordHasher_.performDummyHash();
        co_return Result<AuthTokens>::failure(ErrorCode::Unauthenticated, INVALID_CREDENTIALS);
    }

    if (!passwordHasher_.verify(password, user->passwordHash)) {
        co_return Result<AuthTokens>::failure(ErrorCode::Unauthenticated, INVALID_CREDENTIALS);
    }

    if (!user->isActive) {
        co_return Result<AuthTokens>::failure(ErrorCode::Forbidden,
                                              "this account has been disabled");
    }

    if (settings_.requireVerifiedEmail && !user->emailVerified) {
        co_return Result<AuthTokens>::failure(ErrorCode::Forbidden,
                                              "confirm your email address before signing in");
    }

    // Transparently upgrade a hash produced with weaker parameters, now that the plaintext
    // is available and already proven correct.
    if (passwordHasher_.needsRehash(user->passwordHash)) {
        if (auto upgraded = passwordHasher_.hash(password); upgraded.ok()) {
            co_await users_.updatePasswordHash(user->id, std::move(upgraded).value());
        }
    }

    co_await users_.recordSuccessfulLogin(user->id);

    co_return Result<AuthTokens>::success(
        co_await issueSession(*user, common::randomUuid(), std::nullopt, std::move(client)));
}

drogon::Task<Result<AuthTokens>> AuthService::refresh(std::string refreshToken,
                                                      ClientContext client) const {
    const auto stored = co_await refreshTokens_.findByHash(common::sha256Hex(refreshToken));
    if (!stored.has_value()) {
        co_return Result<AuthTokens>::failure(ErrorCode::Unauthenticated, INVALID_REFRESH_TOKEN);
    }

    if (stored->revoked) {
        // The token was already rotated. Either it leaked and someone is replaying it, or
        // the legitimate client is replaying its own old token; both mean the family can no
        // longer be trusted, so every descendant of that login is burned.
        spdlog::warn("refresh token reuse detected userId={} familyId={}; revoking the family",
                     common::escapeJson(stored->userId),
                     common::escapeJson(stored->familyId));
        co_await refreshTokens_.revokeFamily(stored->familyId);
        co_return Result<AuthTokens>::failure(ErrorCode::Unauthenticated, INVALID_REFRESH_TOKEN);
    }

    if (stored->expired) {
        co_await refreshTokens_.revokeById(stored->id);
        co_return Result<AuthTokens>::failure(ErrorCode::Unauthenticated, INVALID_REFRESH_TOKEN);
    }

    const auto user = co_await users_.findById(stored->userId);
    if (!user.has_value()) {
        co_await refreshTokens_.revokeFamily(stored->familyId);
        co_return Result<AuthTokens>::failure(ErrorCode::Unauthenticated, INVALID_REFRESH_TOKEN);
    }

    if (!user->isActive) {
        co_await refreshTokens_.revokeAllForUser(user->id);
        co_return Result<AuthTokens>::failure(ErrorCode::Forbidden,
                                              "this account has been disabled");
    }

    co_return Result<AuthTokens>::success(
        co_await issueSession(*user, stored->familyId, stored->id, std::move(client)));
}

drogon::Task<VoidResult> AuthService::logout(std::string refreshToken) const {
    const auto stored = co_await refreshTokens_.findByHash(common::sha256Hex(refreshToken));
    if (stored.has_value()) {
        co_await refreshTokens_.revokeById(stored->id);
    }

    // Reporting success for an unknown token keeps the endpoint from confirming whether a
    // token exists.
    co_return VoidResult::success();
}

drogon::Task<VoidResult> AuthService::verifyEmail(std::string token) const {
    const auto userId =
        co_await userTokens_.consume(common::sha256Hex(token), UserTokenPurpose::EmailVerification);
    if (!userId.has_value()) {
        co_return VoidResult::failure(ErrorCode::InvalidInput,
                                      "this verification link is invalid or has expired");
    }

    co_await users_.markEmailVerified(*userId);
    spdlog::info("verified email for user id={}", common::escapeJson(*userId));
    co_return VoidResult::success();
}

drogon::Task<VoidResult> AuthService::requestPasswordReset(std::string email) const {
    const auto user = co_await users_.findByEmail(domain::normalizeEmail(email));
    if (!user.has_value() || !settings_.mailEnabled) {
        // Deliberately a success that sends nothing: an unauthenticated endpoint that reports
        // "no such user" is an account enumeration tool.
        co_return VoidResult::success();
    }

    // Requesting a new link invalidates any earlier one, so only the most recent works.
    co_await userTokens_.invalidateAll(user->id, UserTokenPurpose::PasswordReset);

    const auto token = common::randomUrlSafeToken();
    co_await userTokens_.issue(user->id,
                               UserTokenPurpose::PasswordReset,
                               common::sha256Hex(token),
                               settings_.passwordResetTtl);

    auto message = passwordResetMessage(settings_.mail, user->email, user->displayName, token);
    if (const auto delivery = co_await mailSender_.send(std::move(message)); !delivery.ok()) {
        spdlog::error("could not send the password reset message for user id={}: {}",
                      common::escapeJson(user->id),
                      common::escapeJson(delivery.error().detail));
    }

    co_return VoidResult::success();
}

drogon::Task<VoidResult> AuthService::resetPassword(std::string token,
                                                    std::string newPassword) const {
    if (auto check = domain::validatePassword(newPassword); !check.ok()) {
        co_return check;
    }

    const auto userId =
        co_await userTokens_.consume(common::sha256Hex(token), UserTokenPurpose::PasswordReset);
    if (!userId.has_value()) {
        co_return VoidResult::failure(ErrorCode::InvalidInput,
                                      "this reset link is invalid or has expired");
    }

    auto passwordHash = passwordHasher_.hash(newPassword);
    if (!passwordHash.ok()) {
        co_return VoidResult::failure(passwordHash.error());
    }

    co_await users_.updatePasswordHash(*userId, std::move(passwordHash).value());

    // A password reset is the recovery path after a compromise, so every existing session
    // has to die with it.
    co_await refreshTokens_.revokeAllForUser(*userId);
    co_await userTokens_.invalidateAll(*userId, UserTokenPurpose::PasswordReset);

    spdlog::info("password reset completed for user id={}", common::escapeJson(*userId));
    co_return VoidResult::success();
}

drogon::Task<AuthTokens> AuthService::issueSession(domain::User user,
                                                   std::string familyId,
                                                   std::optional<std::string> rotatesTokenId,
                                                   ClientContext client) const {
    const auto permissions = co_await roles_.permissionsForUser(user.id);

    const auto refreshToken = common::randomUrlSafeToken();
    repositories::NewRefreshToken record{user.id,
                                         familyId,
                                         common::sha256Hex(refreshToken),
                                         settings_.refreshTokenTtl,
                                         client.userAgent,
                                         client.ipAddress};

    if (rotatesTokenId.has_value()) {
        co_await refreshTokens_.rotate(*rotatesTokenId, std::move(record));
    } else {
        co_await refreshTokens_.issue(std::move(record));
    }

    AuthTokens tokens;
    tokens.accessToken =
        tokenService_.issueAccessToken(AccessTokenClaims{user.id, user.email, permissions});
    tokens.refreshToken = refreshToken;
    tokens.accessTokenExpiresIn = tokenService_.accessTokenTtl();
    tokens.permissions = permissions;
    tokens.user = std::move(user);

    co_return tokens;
}

} // namespace launcher::services
