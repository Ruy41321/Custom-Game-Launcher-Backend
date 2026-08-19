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
#include "services/IMailSender.h"
#include "services/MailTemplates.h"
#include "services/PasswordHasher.h"
#include "services/TokenService.h"

namespace launcher::services {

struct AuthSettings {
    /// When true, an account cannot log in until its email address is confirmed.
    bool requireVerifiedEmail{true};
    std::chrono::seconds refreshTokenTtl{2592000};
    std::chrono::seconds emailVerificationTtl{86400};
    std::chrono::seconds passwordResetTtl{3600};
    std::string defaultRole{domain::roles::PLAYER};

    /// Where the links in a message point, and what the deployment calls itself.
    MailContext mail;

    /// False when the deployment sends no mail at all. Nothing is composed and nothing is
    /// handed to the sender, so a disabled transport is quiet rather than an error per
    /// registration. `AppConfig::validate()` refuses to pair it with `requireVerifiedEmail`.
    bool mailEnabled{true};
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
    /// Whether the verification message actually left the building.
    ///
    /// The raw token is deliberately **not** here. It exists for the length of one function,
    /// is handed straight to the sender, and nothing above this layer can return it by
    /// accident — which is the difference between removing the development affordance and
    /// moving it somewhere else.
    bool verificationEmailSent{false};
};

struct AuthTokens {
    std::string accessToken;
    std::string refreshToken;
    std::chrono::seconds accessTokenExpiresIn{0};
    domain::User user;
    std::vector<std::string> permissions;
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
                const IMailSender& mailSender,
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

    /// Issues a fresh verification link and sends it.
    ///
    /// Succeeds for an unknown address, for one that is already verified and for a disabled
    /// account alike, and sends nothing in those three cases: the caller reports one sentence
    /// whatever happened, so the route cannot be used to find out who has an account.
    drogon::Task<common::VoidResult> resendVerification(std::string email) const;

    /// Always succeeds, whether or not the address belongs to an account and whether or not
    /// the message could be delivered. The caller answers the same either way, which is what
    /// keeps this from being an account-enumeration tool; a failed send is a log line, because
    /// there is nothing that can be said about it without saying the address exists.
    drogon::Task<common::VoidResult> requestPasswordReset(std::string email) const;

    drogon::Task<common::VoidResult> resetPassword(std::string token,
                                                   std::string newPassword) const;

    /// Replaces the password of the signed-in account, and hands back a fresh session.
    ///
    /// The way out of a one-time password an operator handed over, and therefore the only
    /// route a `passwordChangeRequired` session reaches. It is an ordinary password change
    /// too: nothing about it is special-cased on the flag, so an account that simply wants a
    /// new password uses the same route.
    ///
    /// Three rules, each with a reason:
    ///
    /// **The current password is asked for again**, exactly as the erasure asks (D44). A valid
    /// access token says who is asking, not that the owner is at the keyboard — and on this
    /// route an unattended session would otherwise be enough to take the account.
    ///
    /// **The new password may not be the old one.** Everywhere else that would be a harmless
    /// no-op; here it is the whole feature defeated, because re-entering the operator's
    /// temporary password would clear the flag and leave a credential somebody else knows.
    ///
    /// **Every other session dies and a new one is issued.** A password change is what somebody
    /// does after a credential leaked, so the sessions minted under the old one cannot survive
    /// it; and the caller needs a token without the flag, which only a new session carries.
    drogon::Task<common::Result<AuthTokens>> changePassword(std::string userId,
                                                            std::string currentPassword,
                                                            std::string newPassword,
                                                            ClientContext client) const;

  private:
    drogon::Task<AuthTokens> issueSession(domain::User user,
                                          std::string familyId,
                                          std::optional<std::string> rotatesTokenId,
                                          ClientContext client) const;

    const repositories::IUserRepository& users_;
    const repositories::IRoleRepository& roles_;
    const repositories::IRefreshTokenRepository& refreshTokens_;
    const repositories::IUserTokenRepository& userTokens_;
    /// Issues a verification token for an account and sends the link. Returns whether the
    /// message went out; false is never a reason to undo anything the caller already did.
    drogon::Task<bool> sendVerificationLink(domain::User user) const;

    const IPasswordHasher& passwordHasher_;
    const ITokenService& tokenService_;
    const IMailSender& mailSender_;
    AuthSettings settings_;
};

} // namespace launcher::services
