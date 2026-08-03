#include "app/AppContext.h"

#include <stdexcept>
#include <utility>

#include "repositories/postgres/PgRepositories.h"

namespace launcher::app {

AppContext& AppContext::instance() {
    static AppContext context;
    return context;
}

void AppContext::initialize(AppConfig config, drogon::orm::DbClientPtr database) {
    config_ = std::move(config);
    database_ = std::move(database);

    users_ = std::make_unique<repositories::postgres::PgUserRepository>(database_);
    roles_ = std::make_unique<repositories::postgres::PgRoleRepository>(database_);
    refreshTokens_ = std::make_unique<repositories::postgres::PgRefreshTokenRepository>(database_);
    userTokens_ = std::make_unique<repositories::postgres::PgUserTokenRepository>(database_);

    passwordHasher_ =
        std::make_unique<services::Argon2idPasswordHasher>(services::PasswordHashingSettings{
            config_.auth.argon2OperationsLimit,
            static_cast<std::size_t>(config_.auth.argon2MemoryLimitBytes)});

    tokenService_ = std::make_unique<services::JwtTokenService>(
        services::TokenSettings{config_.auth.jwtSecret,
                                config_.auth.issuer,
                                std::chrono::seconds{config_.auth.accessTokenTtlSeconds}});

    services::AuthSettings authSettings;
    authSettings.requireVerifiedEmail = config_.auth.requireVerifiedEmail;
    authSettings.refreshTokenTtl = std::chrono::seconds{config_.auth.refreshTokenTtlSeconds};
    authSettings.emailVerificationTtl =
        std::chrono::seconds{config_.auth.emailVerificationTtlSeconds};
    authSettings.passwordResetTtl = std::chrono::seconds{config_.auth.passwordResetTtlSeconds};

    authService_ = std::make_unique<services::AuthService>(*users_,
                                                           *roles_,
                                                           *refreshTokens_,
                                                           *userTokens_,
                                                           *passwordHasher_,
                                                           *tokenService_,
                                                           std::move(authSettings));

    authRateLimiter_ = std::make_unique<common::RateLimiter>(
        config_.rateLimit.authAttempts, std::chrono::seconds{config_.rateLimit.authWindowSeconds});

    initialized_ = true;
}

bool AppContext::initialized() const noexcept {
    return initialized_;
}

void AppContext::requireInitialized() const {
    if (!initialized_) {
        throw std::logic_error("AppContext used before initialize()");
    }
}

const AppConfig& AppContext::config() const {
    requireInitialized();
    return config_;
}

const drogon::orm::DbClientPtr& AppContext::database() const {
    requireInitialized();
    return database_;
}

const services::AuthService& AppContext::authService() const {
    requireInitialized();
    return *authService_;
}

const services::ITokenService& AppContext::tokenService() const {
    requireInitialized();
    return *tokenService_;
}

common::RateLimiter& AppContext::authRateLimiter() const {
    requireInitialized();
    return *authRateLimiter_;
}

void AppContext::reset() {
    initialized_ = false;
    authRateLimiter_.reset();
    authService_.reset();
    tokenService_.reset();
    passwordHasher_.reset();
    userTokens_.reset();
    refreshTokens_.reset();
    roles_.reset();
    users_.reset();
    database_.reset();
    config_ = AppConfig{};
}

} // namespace launcher::app
