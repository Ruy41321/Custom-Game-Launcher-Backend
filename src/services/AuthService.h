#pragma once

#include <drogon/utils/coroutine.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "common/Result.h"
#include "domain/Role.h"
#include "domain/User.h"
#include "repositories/IRefreshTokenRepository.h"
#include "repositories/IRoleRepository.h"
#include "repositories/IUserRepository.h"
#include "repositories/IUserTokenRepository.h"
#include "services/PasswordHasher.h"
#include "services/TokenService.h"

namespace launcher::services {

struct AuthSettings {
    /// When true, an account cannot log in until its email address is confirmed. Off in
    /// development, where there is no mail transport.
    bool requireVerifiedEmail{true};
    std::chrono::seconds refreshTokenTtl{2592000};
    std::chrono::seconds emailVerificationTtl{86400};
    std::chrono::seconds passwordResetTtl{3600};
    std::string defaultRole{domain::roles::PLAYER};
};

/// Where a request came from, recorded against a refresh token so a user can later be shown
/// their active sessions.
struct ClientContext {
    std::string userAgent;
    std::string ipAddress;
};

struct RegisterCommand {
    std::string email;
    std::string password;
    std::string displayName;
};

struct RegistrationResult {
    domain::User user;
    /// The raw verification token, to be delivered by email. It is returned here and never
    /// through the API: only the hash is stored, so this is the one moment it exists.
    std::string emailVerificationToken;
};

struct AuthTokens {
    std::string accessToken;
    std::string refreshToken;
    std::chrono::seconds accessTokenExpiresIn{0};
    domain::User user;
    std::vector<std::string> permissions;
};

struct PasswordResetRequest {
    /// Empty when no account matches. The caller still reports success, so the endpoint
    /// cannot be used to discover which addresses are registered.
    std::optional<std::string> token;
};

/// Registration, login, refresh-token rotation, email verification and password reset.
///
/// Depends only on repository interfaces, so every rule below is unit tested against fakes
/// with no database involved.
class AuthService {
  public:
    AuthService(const repositories::IUserRepository& users,
                const repositories::IRoleRepository& roles,
                const repositories::IRefreshTokenRepository& refreshTokens,
                const repositories::IUserTokenRepository& userTokens,
                const IPasswordHasher& passwordHasher,
                const ITokenService& tokenService,
                AuthSettings settings);

    drogon::Task<common::Result<RegistrationResult>> registerUser(RegisterCommand command) const;

    drogon::Task<common::Result<AuthTokens>>
    login(std::string email, std::string password, ClientContext client) const;

    /// Rotates the presented token. Presenting one that was already rotated is treated as
    /// theft: the whole token family is revoked.
    drogon::Task<common::Result<AuthTokens>> refresh(std::string refreshToken,
                                                     ClientContext client) const;

    /// Idempotent, and succeeds even for an unknown token — a logout endpoint must not
    /// double as a token oracle.
    drogon::Task<common::VoidResult> logout(std::string refreshToken) const;

    drogon::Task<common::VoidResult> verifyEmail(std::string token) const;

    drogon::Task<common::Result<PasswordResetRequest>>
    requestPasswordReset(std::string email) const;

    drogon::Task<common::VoidResult> resetPassword(std::string token,
                                                   std::string newPassword) const;

  private:
    drogon::Task<AuthTokens> issueSession(domain::User user,
                                          std::string familyId,
                                          std::optional<std::string> rotatesTokenId,
                                          ClientContext client) const;

    const repositories::IUserRepository& users_;
    const repositories::IRoleRepository& roles_;
    const repositories::IRefreshTokenRepository& refreshTokens_;
    const repositories::IUserTokenRepository& userTokens_;
    const IPasswordHasher& passwordHasher_;
    const ITokenService& tokenService_;
    AuthSettings settings_;
};

} // namespace launcher::services
