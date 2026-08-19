#include "app/AppContext.h"

#include <stdexcept>
#include <utility>

#include "repositories/postgres/PgAdminRepositories.h"
#include "repositories/postgres/PgCatalogRepositories.h"
#include "repositories/postgres/PgCrashReportRepository.h"
#include "repositories/postgres/PgDownloadRepositories.h"
#include "repositories/postgres/PgLauncherReleaseRepository.h"
#include "repositories/postgres/PgRepositories.h"
#include "repositories/postgres/PgUploadRepositories.h"
#include "services/DisabledMailSender.h"
#include "services/LoggingMailSender.h"
#include "services/SmtpMailSender.h"

namespace launcher::app {
namespace {

services::SmtpSecurity smtpSecurityOf(MailSecurity security) {
    switch (security) {
    case MailSecurity::None:
        return services::SmtpSecurity::None;
    case MailSecurity::Tls:
        return services::SmtpSecurity::Tls;
    case MailSecurity::StartTls:
        break;
    }
    return services::SmtpSecurity::StartTls;
}

std::unique_ptr<services::IMailSender> mailSenderFor(const MailConfig& config) {
    switch (config.transport) {
    case MailTransport::Log:
        return std::make_unique<services::LoggingMailSender>();
    case MailTransport::None:
        return std::make_unique<services::DisabledMailSender>();
    case MailTransport::Smtp:
        break;
    }

    services::SmtpSettings settings;
    settings.host = config.host;
    settings.port = config.port;
    settings.username = config.username;
    settings.password = config.password;
    settings.security = smtpSecurityOf(config.security);
    settings.fromAddress = config.fromAddress;
    settings.fromName = config.fromName;
    settings.timeout = std::chrono::seconds{config.timeoutSeconds};
    return std::make_unique<services::SmtpMailSender>(std::move(settings));
}

} // namespace

AppContext& AppContext::instance() {
    static AppContext context;
    return context;
}

void AppContext::initialize(AppConfig config,
                            drogon::orm::DbClientPtr database,
                            std::unique_ptr<services::IMailSender> mailSender) {
    config_ = std::move(config);
    database_ = std::move(database);
    mailSender_ = mailSender ? std::move(mailSender) : mailSenderFor(config_.mail);

    users_ = std::make_unique<repositories::postgres::PgUserRepository>(database_);
    accounts_ = std::make_unique<repositories::postgres::PgAccountRepository>(database_);
    roles_ = std::make_unique<repositories::postgres::PgRoleRepository>(database_);
    refreshTokens_ = std::make_unique<repositories::postgres::PgRefreshTokenRepository>(database_);
    userTokens_ = std::make_unique<repositories::postgres::PgUserTokenRepository>(database_);
    games_ = std::make_unique<repositories::postgres::PgGameRepository>(database_);
    gameVersions_ = std::make_unique<repositories::postgres::PgGameVersionRepository>(database_);
    builds_ = std::make_unique<repositories::postgres::PgBuildRepository>(database_);
    library_ = std::make_unique<repositories::postgres::PgLibraryRepository>(database_);
    media_ = std::make_unique<repositories::postgres::PgMediaRepository>(database_);
    patchNotes_ = std::make_unique<repositories::postgres::PgPatchNoteRepository>(database_);
    adminUsers_ = std::make_unique<repositories::postgres::PgAdminUserRepository>(database_);
    analytics_ = std::make_unique<repositories::postgres::PgAnalyticsRepository>(database_);
    audit_ = std::make_unique<repositories::postgres::PgAuditRepository>(database_);
    blobs_ = std::make_unique<repositories::postgres::PgBlobRepository>(database_);
    uploadSessions_ =
        std::make_unique<repositories::postgres::PgUploadSessionRepository>(database_);
    downloads_ = std::make_unique<repositories::postgres::PgDownloadRepository>(database_);
    crashReports_ = std::make_unique<repositories::postgres::PgCrashReportRepository>(database_);
    launcherReleases_ =
        std::make_unique<repositories::postgres::PgLauncherReleaseRepository>(database_);

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
    authSettings.mailEnabled = config_.mail.enabled();
    authSettings.mail.linkBaseUrl = config_.mail.linkBaseUrl;
    authSettings.mail.productName = config_.mail.productName;

    authService_ = std::make_unique<services::AuthService>(*users_,
                                                           *roles_,
                                                           *refreshTokens_,
                                                           *userTokens_,
                                                           *passwordHasher_,
                                                           *tokenService_,
                                                           *mailSender_,
                                                           std::move(authSettings));

    // One reclaimer, copied into both services that delete artwork, so the "ask before removing
    // a shared file" rule has exactly one implementation.
    const services::MediaReclaimer artworkReclaimer{
        *media_, storage::MediaStore{std::filesystem::path{config_.media.root}}};

    // Reads the account through IUserRepository and changes it through IAccountRepository:
    // the erasing statement carries its own audit entry, which registration has no business
    // carrying — the same split D36 makes for the administrative surface.
    accountService_ = std::make_unique<services::AccountService>(
        *users_, *accounts_, *adminUsers_, *passwordHasher_);

    catalogService_ = std::make_unique<services::CatalogService>(
        *games_, *gameVersions_, *builds_, *library_, *media_, artworkReclaimer);

    patchNoteService_ =
        std::make_unique<services::PatchNoteService>(*games_, *gameVersions_, *patchNotes_);
    adminUserService_ =
        std::make_unique<services::AdminUserService>(*adminUsers_, *audit_, *passwordHasher_);
    analyticsService_ = std::make_unique<services::AnalyticsService>(*analytics_);

    mediaService_ = std::make_unique<services::MediaService>(
        *games_,
        *media_,
        storage::MediaStore{std::filesystem::path{config_.media.root}},
        services::MediaLimits{config_.media.maxBytes, config_.media.maxVideoBytes});

    services::UploadSettings uploadSettings;
    uploadSettings.maxBlobBytes = config_.uploads.maxBlobBytes;
    uploadSettings.maxChunkBytes = config_.uploads.maxChunkBytes;
    uploadSettings.sessionTtl = std::chrono::seconds{config_.uploads.sessionTtlSeconds};
    uploadSettings.maxOpenSessionsPerUser = config_.uploads.maxOpenSessionsPerUser;

    uploadService_ = std::make_unique<services::UploadService>(
        *builds_,
        *blobs_,
        *uploadSessions_,
        *users_,
        storage::BlobStore{std::filesystem::path{config_.storage.blobRoot}},
        std::move(uploadSettings));

    storage::SignedUrlSettings urlSettings;
    urlSettings.publicBaseUrl = config_.storage.publicBaseUrl;
    urlSettings.secret = config_.storage.secureLinkSecret;
    urlSettings.ttl = std::chrono::seconds{config_.storage.signedUrlTtlSeconds};

    downloadService_ = std::make_unique<services::DownloadService>(
        *builds_,
        *downloads_,
        storage::DownloadUrlSigner{std::move(urlSettings)},
        services::DownloadSettings{config_.updates.fullDownloadThresholdRatio});

    services::RetentionSettings retentionSettings;
    retentionSettings.blobGraceSeconds = config_.retention.blobGraceSeconds;
    retentionSettings.batchSize = config_.retention.sweepBatchSize;

    retentionService_ = std::make_unique<services::RetentionService>(
        *blobs_,
        *users_,
        storage::BlobStore{std::filesystem::path{config_.storage.blobRoot}},
        retentionSettings);

    crashReportService_ = std::make_unique<services::CrashReportService>(
        *crashReports_, services::CrashReportSettings{config_.crashReports.retentionSeconds});

    launcherReleaseService_ = std::make_unique<services::LauncherReleaseService>(
        *launcherReleases_,
        services::LauncherReleaseSettings{config_.launcherReleases.publicKey,
                                          config_.launcherReleases.publicBaseUrl});

    crashRateLimiter_ = std::make_unique<common::RateLimiter>(
        config_.crashReports.submitAttempts,
        std::chrono::seconds{config_.crashReports.submitWindowSeconds});

    authRateLimiter_ = std::make_unique<common::RateLimiter>(
        config_.rateLimit.authAttempts, std::chrono::seconds{config_.rateLimit.authWindowSeconds});

    accountRateLimiter_ = std::make_unique<common::RateLimiter>(
        config_.rateLimit.accountRequests,
        std::chrono::seconds{config_.rateLimit.accountWindowSeconds});

    mailRateLimiter_ = std::make_unique<common::RateLimiter>(
        config_.mail.sendAttempts, std::chrono::seconds{config_.mail.sendWindowSeconds});

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

const services::AccountService& AppContext::accountService() const {
    requireInitialized();
    return *accountService_;
}

const services::CatalogService& AppContext::catalogService() const {
    requireInitialized();
    return *catalogService_;
}

const services::RetentionService& AppContext::retentionService() const {
    requireInitialized();
    return *retentionService_;
}

const services::AnalyticsService& AppContext::analyticsService() const {
    requireInitialized();
    return *analyticsService_;
}

const services::AdminUserService& AppContext::adminUserService() const {
    requireInitialized();
    return *adminUserService_;
}

const services::PatchNoteService& AppContext::patchNoteService() const {
    requireInitialized();
    return *patchNoteService_;
}

const services::MediaService& AppContext::mediaService() const {
    requireInitialized();
    return *mediaService_;
}

const services::UploadService& AppContext::uploadService() const {
    requireInitialized();
    return *uploadService_;
}

const services::DownloadService& AppContext::downloadService() const {
    requireInitialized();
    return *downloadService_;
}

const services::CrashReportService& AppContext::crashReportService() const {
    requireInitialized();
    return *crashReportService_;
}

const services::LauncherReleaseService& AppContext::launcherReleaseService() const {
    requireInitialized();
    return *launcherReleaseService_;
}

common::RateLimiter& AppContext::authRateLimiter() const {
    requireInitialized();
    return *authRateLimiter_;
}

common::RateLimiter& AppContext::crashRateLimiter() const {
    requireInitialized();
    return *crashRateLimiter_;
}

common::RateLimiter& AppContext::accountRateLimiter() const {
    requireInitialized();
    return *accountRateLimiter_;
}

common::RateLimiter& AppContext::mailRateLimiter() const {
    requireInitialized();
    return *mailRateLimiter_;
}

void AppContext::reset() {
    initialized_ = false;
    mailRateLimiter_.reset();
    authRateLimiter_.reset();
    crashRateLimiter_.reset();
    accountRateLimiter_.reset();
    launcherReleaseService_.reset();
    crashReportService_.reset();
    retentionService_.reset();
    downloadService_.reset();
    uploadService_.reset();
    patchNoteService_.reset();
    mediaService_.reset();
    analyticsService_.reset();
    adminUserService_.reset();
    catalogService_.reset();
    authService_.reset();
    accountService_.reset();
    tokenService_.reset();
    passwordHasher_.reset();
    mailSender_.reset();
    launcherReleases_.reset();
    crashReports_.reset();
    downloads_.reset();
    uploadSessions_.reset();
    blobs_.reset();
    audit_.reset();
    analytics_.reset();
    adminUsers_.reset();
    patchNotes_.reset();
    media_.reset();
    library_.reset();
    builds_.reset();
    gameVersions_.reset();
    games_.reset();
    userTokens_.reset();
    refreshTokens_.reset();
    roles_.reset();
    accounts_.reset();
    users_.reset();
    database_.reset();
    config_ = AppConfig{};
}

} // namespace launcher::app
