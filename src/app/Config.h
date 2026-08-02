#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "common/EnvInterpolation.h"
#include "common/Result.h"

namespace launcher::app {

struct ServerConfig {
    std::string listenAddress{"0.0.0.0"};
    uint16_t port{8080};
    /// The admin surface is deliberately a second listener bound to the loopback interface;
    /// it is reached over an SSH tunnel and is never exposed publicly.
    std::string adminListenAddress{"127.0.0.1"};
    uint16_t adminPort{9090};
    bool adminEnabled{false};
    size_t threadCount{0}; ///< 0 = one event loop per hardware thread
};

struct DatabaseConfig {
    std::string host{"localhost"};
    uint16_t port{5432};
    std::string name{"launcher"};
    std::string user{"launcher"};
    std::string password;
    size_t connectionCount{4};

    std::string connectionString() const;
};

struct LoggingConfig {
    std::string level{"info"};
    std::string directory;
    bool json{true};
};

struct StorageConfig {
    std::string blobRoot{"/data/blobs"};
    std::string publicBaseUrl{"http://localhost:8081/files"};
    std::string secureLinkSecret;
    uint32_t signedUrlTtlSeconds{3600};
};

struct AuthConfig {
    std::string jwtSecret;
    std::string issuer{"custom-game-launcher"};
    uint32_t accessTokenTtlSeconds{900};
    uint32_t refreshTokenTtlSeconds{2592000};
};

struct UpdateConfig {
    /// When deltaBytes / fullBytes exceeds this ratio the server advises a full download
    /// instead of a delta. See CLAUDE.md §3.
    double fullDownloadThresholdRatio{0.7};
};

struct UploadConfig {
    int64_t defaultQuotaBytes{5LL * 1024 * 1024 * 1024};
};

struct AppConfig {
    std::string environment{"development"};
    std::string migrationsDirectory{"migrations"};
    ServerConfig server;
    DatabaseConfig database;
    LoggingConfig logging;
    StorageConfig storage;
    AuthConfig auth;
    UpdateConfig updates;
    UploadConfig uploads;

    bool isProduction() const;

    /// Parses configuration from a JSON document, expanding `${VAR}` placeholders through
    /// `lookup`. Kept separate from file loading so it can be unit tested without touching
    /// the filesystem or the process environment.
    static common::Result<AppConfig> parse(std::string_view json, const common::EnvLookup& lookup);

    static common::Result<AppConfig> loadFromFile(const std::filesystem::path& path,
                                                  const common::EnvLookup& lookup);

    /// Rejects configurations that are unsafe to run, e.g. a production deployment with no
    /// JWT secret. Called by both loaders.
    common::VoidResult validate() const;
};

/// Resolves which configuration file to use: an explicit path wins, otherwise
/// `$LAUNCHER_CONFIG`, otherwise `config/config.<$LAUNCHER_ENV or development>.json`.
std::filesystem::path resolveConfigPath(const std::string& explicitPath,
                                        const common::EnvLookup& lookup);

} // namespace launcher::app
