#pragma once

#include <drogon/orm/DbClient.h>

#include <memory>

#include "app/Config.h"
#include "common/RateLimiter.h"
#include "repositories/IRefreshTokenRepository.h"
#include "repositories/IRoleRepository.h"
#include "repositories/IUserRepository.h"
#include "repositories/IUserTokenRepository.h"
#include "services/AuthService.h"
#include "services/PasswordHasher.h"
#include "services/TokenService.h"

namespace launcher::app {

/// Composition root.
///
/// Drogon instantiates controllers and filters itself, so there is no place to inject
/// dependencies into them. This context is built once during startup, constructs the
/// concrete PostgreSQL repositories, and injects them into the services behind their
/// interfaces. Controllers read services from here; services themselves never name a
/// concrete repository, which is what keeps them unit-testable against fakes.
class AppContext {
  public:
    static AppContext& instance();

    void initialize(AppConfig config, drogon::orm::DbClientPtr database);

    bool initialized() const noexcept;

    const AppConfig& config() const;

    const drogon::orm::DbClientPtr& database() const;

    const services::AuthService& authService() const;

    const services::ITokenService& tokenService() const;

    /// Shared by the authentication endpoints; see common::RateLimiter for why it is
    /// in-process.
    common::RateLimiter& authRateLimiter() const;

    /// Test hook: drops all wiring so a fresh context can be installed.
    void reset();

    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;
    AppContext(AppContext&&) = delete;
    AppContext& operator=(AppContext&&) = delete;

  private:
    AppContext() = default;
    ~AppContext() = default;

    void requireInitialized() const;

    bool initialized_{false};
    AppConfig config_;
    drogon::orm::DbClientPtr database_;

    std::unique_ptr<repositories::IUserRepository> users_;
    std::unique_ptr<repositories::IRoleRepository> roles_;
    std::unique_ptr<repositories::IRefreshTokenRepository> refreshTokens_;
    std::unique_ptr<repositories::IUserTokenRepository> userTokens_;

    std::unique_ptr<services::IPasswordHasher> passwordHasher_;
    std::unique_ptr<services::ITokenService> tokenService_;
    std::unique_ptr<services::AuthService> authService_;

    std::unique_ptr<common::RateLimiter> authRateLimiter_;
};

} // namespace launcher::app
