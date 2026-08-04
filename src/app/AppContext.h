#pragma once

#include <drogon/orm/DbClient.h>

#include <memory>

#include "app/Config.h"
#include "common/RateLimiter.h"
#include "repositories/IBlobRepository.h"
#include "repositories/IBuildRepository.h"
#include "repositories/IDownloadRepository.h"
#include "repositories/IGameRepository.h"
#include "repositories/IGameVersionRepository.h"
#include "repositories/ILibraryRepository.h"
#include "repositories/IMediaRepository.h"
#include "repositories/IRefreshTokenRepository.h"
#include "repositories/IRoleRepository.h"
#include "repositories/IUploadSessionRepository.h"
#include "repositories/IUserRepository.h"
#include "repositories/IUserTokenRepository.h"
#include "services/AuthService.h"
#include "services/CatalogService.h"
#include "services/DownloadService.h"
#include "services/MediaService.h"
#include "services/PasswordHasher.h"
#include "services/TokenService.h"
#include "services/UploadService.h"

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

    const services::CatalogService& catalogService() const;

    const services::MediaService& mediaService() const;

    const services::UploadService& uploadService() const;

    const services::DownloadService& downloadService() const;

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
    std::unique_ptr<repositories::IGameRepository> games_;
    std::unique_ptr<repositories::IGameVersionRepository> gameVersions_;
    std::unique_ptr<repositories::IBuildRepository> builds_;
    std::unique_ptr<repositories::ILibraryRepository> library_;
    std::unique_ptr<repositories::IMediaRepository> media_;
    std::unique_ptr<repositories::IBlobRepository> blobs_;
    std::unique_ptr<repositories::IUploadSessionRepository> uploadSessions_;
    std::unique_ptr<repositories::IDownloadRepository> downloads_;

    std::unique_ptr<services::IPasswordHasher> passwordHasher_;
    std::unique_ptr<services::ITokenService> tokenService_;
    std::unique_ptr<services::AuthService> authService_;
    std::unique_ptr<services::CatalogService> catalogService_;
    std::unique_ptr<services::MediaService> mediaService_;
    std::unique_ptr<services::UploadService> uploadService_;
    std::unique_ptr<services::DownloadService> downloadService_;

    std::unique_ptr<common::RateLimiter> authRateLimiter_;
};

} // namespace launcher::app
