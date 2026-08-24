#pragma once

#include <drogon/orm/DbClient.h>

#include <memory>

#include "app/Config.h"
#include "common/RateLimiter.h"
#include "repositories/IAccountRepository.h"
#include "repositories/IAdminUserRepository.h"
#include "repositories/IAnalyticsRepository.h"
#include "repositories/IAuditRepository.h"
#include "repositories/IBlobRepository.h"
#include "repositories/IBuildRepository.h"
#include "repositories/ICrashReportRepository.h"
#include "repositories/IDownloadRepository.h"
#include "repositories/IGameRepository.h"
#include "repositories/IGameVersionRepository.h"
#include "repositories/ILauncherReleaseRepository.h"
#include "repositories/ILibraryRepository.h"
#include "repositories/IMediaRepository.h"
#include "repositories/IPatchNoteRepository.h"
#include "repositories/IRefreshTokenRepository.h"
#include "repositories/IRoleRepository.h"
#include "repositories/IUploadSessionRepository.h"
#include "repositories/IUserRepository.h"
#include "repositories/IUserTokenRepository.h"
#include "services/AccountService.h"
#include "services/AdminUserService.h"
#include "services/AnalyticsService.h"
#include "services/AuthService.h"
#include "services/CatalogService.h"
#include "services/CrashReportService.h"
#include "services/DownloadService.h"
#include "services/IMailSender.h"
#include "services/LauncherReleaseService.h"
#include "services/MediaService.h"
#include "services/PasswordHasher.h"
#include "services/PatchNoteService.h"
#include "services/RetentionService.h"
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

    /// Builds everything. `mailSender` is the one dependency that can be handed in, because it
    /// is the one whose real implementation talks to a machine the test suite does not have:
    /// the integration harness installs a capturing sender and reads the link out of the
    /// message, which is how the flows stay exercisable now that no token is returned in a
    /// response. Null means "build the one this configuration describes".
    void initialize(AppConfig config,
                    drogon::orm::DbClientPtr database,
                    std::unique_ptr<services::IMailSender> mailSender = nullptr);

    bool initialized() const noexcept;

    const AppConfig& config() const;

    const drogon::orm::DbClientPtr& database() const;

    const services::AuthService& authService() const;

    const services::AccountService& accountService() const;

    const services::ITokenService& tokenService() const;

    const services::CatalogService& catalogService() const;

    const services::AdminUserService& adminUserService() const;

    const services::AnalyticsService& analyticsService() const;

    const services::MediaService& mediaService() const;

    const services::PatchNoteService& patchNoteService() const;

    const services::UploadService& uploadService() const;

    const services::DownloadService& downloadService() const;

    const services::RetentionService& retentionService() const;

    const services::CrashReportService& crashReportService() const;

    const services::LauncherReleaseService& launcherReleaseService() const;

    /// Shared by the authentication endpoints; see common::RateLimiter for why it is
    /// in-process.
    common::RateLimiter& authRateLimiter() const;

    /// A second bucket, for the unauthenticated crash-report route. Separate from the auth one
    /// on purpose: the two exist for different reasons and want different numbers, and sharing
    /// would let a burst of crash reports lock somebody out of signing in.
    common::RateLimiter& crashRateLimiter() const;

    /// A third bucket, keyed on the *account* rather than the address, for every authenticated
    /// route. The address buckets say nothing about a caller that holds a valid token and moves
    /// between addresses; this one is the ceiling a session has.
    common::RateLimiter& accountRateLimiter() const;

    /// A fourth, for the routes that put a message in somebody's inbox. See MailConfig for why
    /// it is not the authentication one.
    common::RateLimiter& mailRateLimiter() const;

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
    std::unique_ptr<repositories::IAccountRepository> accounts_;
    std::unique_ptr<repositories::IRoleRepository> roles_;
    std::unique_ptr<repositories::IRefreshTokenRepository> refreshTokens_;
    std::unique_ptr<repositories::IUserTokenRepository> userTokens_;
    std::unique_ptr<repositories::IGameRepository> games_;
    std::unique_ptr<repositories::IGameVersionRepository> gameVersions_;
    std::unique_ptr<repositories::IBuildRepository> builds_;
    std::unique_ptr<repositories::ILibraryRepository> library_;
    std::unique_ptr<repositories::IMediaRepository> media_;
    std::unique_ptr<repositories::IPatchNoteRepository> patchNotes_;
    std::unique_ptr<repositories::IAdminUserRepository> adminUsers_;
    std::unique_ptr<repositories::IAnalyticsRepository> analytics_;
    std::unique_ptr<repositories::IAuditRepository> audit_;
    std::unique_ptr<repositories::IBlobRepository> blobs_;
    std::unique_ptr<repositories::IUploadSessionRepository> uploadSessions_;
    std::unique_ptr<repositories::IDownloadRepository> downloads_;
    std::unique_ptr<repositories::ICrashReportRepository> crashReports_;
    std::unique_ptr<repositories::ILauncherReleaseRepository> launcherReleases_;

    std::unique_ptr<services::IMailSender> mailSender_;
    std::unique_ptr<services::IPasswordHasher> passwordHasher_;
    std::unique_ptr<services::ITokenService> tokenService_;
    std::unique_ptr<services::AuthService> authService_;
    std::unique_ptr<services::AccountService> accountService_;
    std::unique_ptr<services::CatalogService> catalogService_;
    std::unique_ptr<services::AdminUserService> adminUserService_;
    std::unique_ptr<services::AnalyticsService> analyticsService_;
    std::unique_ptr<services::MediaService> mediaService_;
    std::unique_ptr<services::PatchNoteService> patchNoteService_;
    std::unique_ptr<services::UploadService> uploadService_;
    std::unique_ptr<services::DownloadService> downloadService_;
    std::unique_ptr<services::RetentionService> retentionService_;
    std::unique_ptr<services::CrashReportService> crashReportService_;
    std::unique_ptr<services::LauncherReleaseService> launcherReleaseService_;

    std::unique_ptr<common::RateLimiter> authRateLimiter_;
    std::unique_ptr<common::RateLimiter> crashRateLimiter_;
    std::unique_ptr<common::RateLimiter> accountRateLimiter_;
    std::unique_ptr<common::RateLimiter> mailRateLimiter_;
};

} // namespace launcher::app
