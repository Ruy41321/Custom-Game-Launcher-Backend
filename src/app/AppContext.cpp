#include "app/AppContext.h"

#include <stdexcept>
#include <utility>

#include "repositories/postgres/PgCatalogRepositories.h"
#include "repositories/postgres/PgDownloadRepositories.h"
#include "repositories/postgres/PgRepositories.h"
#include "repositories/postgres/PgUploadRepositories.h"

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
    games_ = std::make_unique<repositories::postgres::PgGameRepository>(database_);
    gameVersions_ = std::make_unique<repositories::postgres::PgGameVersionRepository>(database_);
    builds_ = std::make_unique<repositories::postgres::PgBuildRepository>(database_);
    library_ = std::make_unique<repositories::postgres::PgLibraryRepository>(database_);
    media_ = std::make_unique<repositories::postgres::PgMediaRepository>(database_);
    blobs_ = std::make_unique<repositories::postgres::PgBlobRepository>(database_);
    uploadSessions_ =
        std::make_unique<repositories::postgres::PgUploadSessionRepository>(database_);
    downloads_ = std::make_unique<repositories::postgres::PgDownloadRepository>(database_);

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

    catalogService_ = std::make_unique<services::CatalogService>(
        *games_, *gameVersions_, *builds_, *library_, *media_);

    mediaService_ = std::make_unique<services::MediaService>(
        *games_,
        *media_,
        storage::MediaStore{std::filesystem::path{config_.media.root}},
        services::MediaLimits{config_.media.maxBytes});

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

const services::CatalogService& AppContext::catalogService() const {
    requireInitialized();
    return *catalogService_;
}

const services::RetentionService& AppContext::retentionService() const {
    requireInitialized();
    return *retentionService_;
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

common::RateLimiter& AppContext::authRateLimiter() const {
    requireInitialized();
    return *authRateLimiter_;
}

void AppContext::reset() {
    initialized_ = false;
    authRateLimiter_.reset();
    retentionService_.reset();
    downloadService_.reset();
    uploadService_.reset();
    mediaService_.reset();
    catalogService_.reset();
    authService_.reset();
    tokenService_.reset();
    passwordHasher_.reset();
    downloads_.reset();
    uploadSessions_.reset();
    blobs_.reset();
    media_.reset();
    library_.reset();
    builds_.reset();
    gameVersions_.reset();
    games_.reset();
    userTokens_.reset();
    refreshTokens_.reset();
    roles_.reset();
    users_.reset();
    database_.reset();
    config_ = AppConfig{};
}

} // namespace launcher::app
