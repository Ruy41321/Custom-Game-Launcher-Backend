#include "app/Config.h"

#include <json/json.h>

#include <fstream>
#include <sstream>

namespace launcher::app {
namespace {

using common::ErrorCode;
using common::Result;

std::string readString(const Json::Value& node, const char* key, const std::string& fallback) {
    return node.isMember(key) && node[key].isString() ? node[key].asString() : fallback;
}

bool readBool(const Json::Value& node, const char* key, bool fallback) {
    return node.isMember(key) && node[key].isBool() ? node[key].asBool() : fallback;
}

double readDouble(const Json::Value& node, const char* key, double fallback) {
    return node.isMember(key) && node[key].isNumeric() ? node[key].asDouble() : fallback;
}

template<typename T>
T readInt(const Json::Value& node, const char* key, T fallback) {
    if (!node.isMember(key) || !node[key].isNumeric()) {
        return fallback;
    }
    return static_cast<T>(node[key].asInt64());
}

std::vector<std::string>
readStrings(const Json::Value& node, const char* key, std::vector<std::string> fallback) {
    if (!node.isMember(key) || !node[key].isArray()) {
        return fallback;
    }
    std::vector<std::string> values;
    for (const auto& element : node[key]) {
        if (element.isString() && !element.asString().empty()) {
            values.push_back(element.asString());
        }
    }
    return values;
}

/// Marks the secrets this repository ships so a deployment can boot with no setup. Their whole
/// point is that they are public, so any environment but development refuses them.
bool isDevelopmentPlaceholder(const std::string& secret) {
    return secret.find("dev-insecure") != std::string::npos;
}

} // namespace

std::string DatabaseConfig::connectionString() const {
    std::ostringstream out;
    out << "host=" << host << " port=" << port << " dbname=" << name << " user=" << user
        << " password=" << password;
    return out.str();
}

bool AppConfig::isProduction() const {
    return environment == "production";
}

Result<AppConfig> AppConfig::parse(std::string_view json, const common::EnvLookup& lookup) {
    auto expanded = common::interpolateEnv(json, lookup);
    if (!expanded.ok()) {
        return Result<AppConfig>::failure(expanded.error());
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string parseErrors;
    const std::string text = std::move(expanded).value();
    std::istringstream stream(text);
    if (!Json::parseFromStream(builder, stream, &root, &parseErrors)) {
        return Result<AppConfig>::failure(ErrorCode::InvalidInput,
                                          "invalid configuration JSON: " + parseErrors);
    }
    if (!root.isObject()) {
        return Result<AppConfig>::failure(ErrorCode::InvalidInput,
                                          "configuration root must be a JSON object");
    }

    AppConfig config;
    config.environment = readString(root, "environment", config.environment);
    config.migrationsDirectory =
        readString(root, "migrationsDirectory", config.migrationsDirectory);

    const auto& server = root["server"];
    config.server.listenAddress = readString(server, "listenAddress", config.server.listenAddress);
    config.server.port = readInt<uint16_t>(server, "port", config.server.port);
    config.server.adminListenAddress =
        readString(server, "adminListenAddress", config.server.adminListenAddress);
    config.server.adminPort = readInt<uint16_t>(server, "adminPort", config.server.adminPort);
    config.server.adminEnabled = readBool(server, "adminEnabled", config.server.adminEnabled);
    config.server.threadCount = readInt<size_t>(server, "threadCount", config.server.threadCount);
    config.server.maxDocumentBytes =
        readInt<int64_t>(server, "maxDocumentBytes", config.server.maxDocumentBytes);
    config.server.maxAnonymousBodyBytes =
        readInt<int64_t>(server, "maxAnonymousBodyBytes", config.server.maxAnonymousBodyBytes);
    config.server.trustedProxies =
        readStrings(server, "trustedProxies", config.server.trustedProxies);

    const auto& security = root["security"];
    config.security.hsts = readBool(security, "hsts", config.security.hsts);
    config.security.hstsMaxAgeSeconds =
        readInt<uint32_t>(security, "hstsMaxAgeSeconds", config.security.hstsMaxAgeSeconds);

    const auto& database = root["database"];
    config.database.host = readString(database, "host", config.database.host);
    config.database.port = readInt<uint16_t>(database, "port", config.database.port);
    config.database.name = readString(database, "name", config.database.name);
    config.database.user = readString(database, "user", config.database.user);
    config.database.password = readString(database, "password", config.database.password);
    config.database.connectionCount =
        readInt<size_t>(database, "connectionCount", config.database.connectionCount);

    const auto& logging = root["logging"];
    config.logging.level = readString(logging, "level", config.logging.level);
    config.logging.directory = readString(logging, "directory", config.logging.directory);
    config.logging.json = readBool(logging, "json", config.logging.json);

    const auto& storage = root["storage"];
    config.storage.blobRoot = readString(storage, "blobRoot", config.storage.blobRoot);
    config.storage.publicBaseUrl =
        readString(storage, "publicBaseUrl", config.storage.publicBaseUrl);
    config.storage.secureLinkSecret =
        readString(storage, "secureLinkSecret", config.storage.secureLinkSecret);
    config.storage.signedUrlTtlSeconds =
        readInt<uint32_t>(storage, "signedUrlTtlSeconds", config.storage.signedUrlTtlSeconds);

    const auto& media = root["media"];
    config.media.root = readString(media, "root", config.media.root);
    config.media.publicBaseUrl = readString(media, "publicBaseUrl", config.media.publicBaseUrl);
    config.media.maxBytes = readInt<int64_t>(media, "maxBytes", config.media.maxBytes);

    const auto& retention = root["retention"];
    config.retention.blobGraceSeconds =
        readInt<uint32_t>(retention, "blobGraceSeconds", config.retention.blobGraceSeconds);
    config.retention.sweepIntervalSeconds =
        readInt<uint32_t>(retention, "sweepIntervalSeconds", config.retention.sweepIntervalSeconds);
    config.retention.sweepBatchSize =
        readInt<int32_t>(retention, "sweepBatchSize", config.retention.sweepBatchSize);

    const auto& crashReports = root["crashReports"];
    config.crashReports.enabled = readBool(crashReports, "enabled", config.crashReports.enabled);
    config.crashReports.submitAttempts =
        readInt<uint32_t>(crashReports, "submitAttempts", config.crashReports.submitAttempts);
    config.crashReports.submitWindowSeconds = readInt<uint32_t>(
        crashReports, "submitWindowSeconds", config.crashReports.submitWindowSeconds);
    config.crashReports.retentionSeconds =
        readInt<uint32_t>(crashReports, "retentionSeconds", config.crashReports.retentionSeconds);
    config.crashReports.sweepIntervalSeconds = readInt<uint32_t>(
        crashReports, "sweepIntervalSeconds", config.crashReports.sweepIntervalSeconds);

    const auto& auth = root["auth"];
    config.auth.jwtSecret = readString(auth, "jwtSecret", config.auth.jwtSecret);
    config.auth.issuer = readString(auth, "issuer", config.auth.issuer);
    config.auth.accessTokenTtlSeconds =
        readInt<uint32_t>(auth, "accessTokenTtlSeconds", config.auth.accessTokenTtlSeconds);
    config.auth.refreshTokenTtlSeconds =
        readInt<uint32_t>(auth, "refreshTokenTtlSeconds", config.auth.refreshTokenTtlSeconds);
    config.auth.emailVerificationTtlSeconds = readInt<uint32_t>(
        auth, "emailVerificationTtlSeconds", config.auth.emailVerificationTtlSeconds);
    config.auth.passwordResetTtlSeconds =
        readInt<uint32_t>(auth, "passwordResetTtlSeconds", config.auth.passwordResetTtlSeconds);
    config.auth.requireVerifiedEmail =
        readBool(auth, "requireVerifiedEmail", config.auth.requireVerifiedEmail);
    config.auth.argon2OperationsLimit =
        readInt<uint64_t>(auth, "argon2OperationsLimit", config.auth.argon2OperationsLimit);
    config.auth.argon2MemoryLimitBytes =
        readInt<uint64_t>(auth, "argon2MemoryLimitBytes", config.auth.argon2MemoryLimitBytes);

    const auto& rateLimit = root["rateLimit"];
    config.rateLimit.authAttempts =
        readInt<uint32_t>(rateLimit, "authAttempts", config.rateLimit.authAttempts);
    config.rateLimit.authWindowSeconds =
        readInt<uint32_t>(rateLimit, "authWindowSeconds", config.rateLimit.authWindowSeconds);
    config.rateLimit.accountRequests =
        readInt<uint32_t>(rateLimit, "accountRequests", config.rateLimit.accountRequests);
    config.rateLimit.accountWindowSeconds =
        readInt<uint32_t>(rateLimit, "accountWindowSeconds", config.rateLimit.accountWindowSeconds);

    const auto& updates = root["updates"];
    config.updates.fullDownloadThresholdRatio = readDouble(
        updates, "fullDownloadThresholdRatio", config.updates.fullDownloadThresholdRatio);

    const auto& uploads = root["uploads"];
    config.uploads.defaultQuotaBytes =
        readInt<int64_t>(uploads, "defaultQuotaBytes", config.uploads.defaultQuotaBytes);
    config.uploads.maxBlobBytes =
        readInt<int64_t>(uploads, "maxBlobBytes", config.uploads.maxBlobBytes);
    config.uploads.maxChunkBytes =
        readInt<int64_t>(uploads, "maxChunkBytes", config.uploads.maxChunkBytes);
    config.uploads.sessionTtlSeconds =
        readInt<uint32_t>(uploads, "sessionTtlSeconds", config.uploads.sessionTtlSeconds);
    config.uploads.maxOpenSessionsPerUser =
        readInt<int64_t>(uploads, "maxOpenSessionsPerUser", config.uploads.maxOpenSessionsPerUser);
    config.uploads.sweepIntervalSeconds =
        readInt<uint32_t>(uploads, "sweepIntervalSeconds", config.uploads.sweepIntervalSeconds);

    if (auto validation = config.validate(); !validation.ok()) {
        return Result<AppConfig>::failure(validation.error());
    }

    return Result<AppConfig>::success(std::move(config));
}

Result<AppConfig> AppConfig::loadFromFile(const std::filesystem::path& path,
                                          const common::EnvLookup& lookup) {
    std::ifstream file(path);
    if (!file) {
        return Result<AppConfig>::failure(ErrorCode::NotFound,
                                          "configuration file not found: " + path.string());
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return parse(buffer.str(), lookup);
}

common::VoidResult AppConfig::validate() const {
    if (server.port == 0) {
        return common::VoidResult::failure(ErrorCode::InvalidInput, "server.port must be non-zero");
    }
    if (server.adminEnabled && server.adminPort == server.port) {
        // The administrative routes are hidden from the public surface by the port a request
        // arrived on. Give them the same port and that check passes for everybody, which turns
        // user management into a public endpoint guarded by nothing but a permission claim.
        return common::VoidResult::failure(
            ErrorCode::InvalidInput,
            "server.adminPort must differ from server.port: the admin surface is separated by "
            "the listener a request arrives on");
    }
    if (server.adminEnabled && server.adminPort == 0) {
        return common::VoidResult::failure(ErrorCode::InvalidInput,
                                           "server.adminPort must be non-zero when the admin "
                                           "surface is enabled");
    }
    if (database.name.empty() || database.user.empty()) {
        return common::VoidResult::failure(ErrorCode::InvalidInput,
                                           "database.name and database.user are required");
    }
    if (updates.fullDownloadThresholdRatio <= 0.0 || updates.fullDownloadThresholdRatio > 1.0) {
        return common::VoidResult::failure(
            ErrorCode::InvalidInput, "updates.fullDownloadThresholdRatio must be within (0, 1]");
    }
    if (uploads.defaultQuotaBytes <= 0) {
        return common::VoidResult::failure(ErrorCode::InvalidInput,
                                           "uploads.defaultQuotaBytes must be positive");
    }
    if (uploads.maxBlobBytes <= 0 || uploads.maxChunkBytes <= 0) {
        return common::VoidResult::failure(ErrorCode::InvalidInput,
                                           "uploads.maxBlobBytes and uploads.maxChunkBytes must be "
                                           "positive");
    }
    if (uploads.maxChunkBytes > uploads.maxBlobBytes) {
        return common::VoidResult::failure(
            ErrorCode::InvalidInput,
            "uploads.maxChunkBytes cannot exceed uploads.maxBlobBytes: a chunk is part of a file");
    }
    if (server.maxDocumentBytes <= 0 || server.maxAnonymousBodyBytes <= 0) {
        return common::VoidResult::failure(
            ErrorCode::InvalidInput,
            "server.maxDocumentBytes and server.maxAnonymousBodyBytes must be positive");
    }
    if (server.maxAnonymousBodyBytes > server.maxDocumentBytes) {
        return common::VoidResult::failure(ErrorCode::InvalidInput,
                                           "server.maxAnonymousBodyBytes cannot exceed "
                                           "server.maxDocumentBytes: it is the tighter of the two");
    }
    if (rateLimit.authAttempts == 0 || rateLimit.authWindowSeconds == 0 ||
        rateLimit.accountRequests == 0 || rateLimit.accountWindowSeconds == 0) {
        // A zero bucket refuses everybody, which reads as an outage rather than as a throttle.
        // Turning a limit off is not a supported configuration; widening it is.
        return common::VoidResult::failure(ErrorCode::InvalidInput,
                                           "every rateLimit value must be positive");
    }
    if (uploads.sessionTtlSeconds == 0 || uploads.maxOpenSessionsPerUser <= 0) {
        return common::VoidResult::failure(
            ErrorCode::InvalidInput,
            "uploads.sessionTtlSeconds and uploads.maxOpenSessionsPerUser "
            "must be positive");
    }

    // Secrets are allowed to be blank in development so the stack boots with no setup, but
    // never in a deployed environment.
    if (environment != "development") {
        if (auth.jwtSecret.size() < 32) {
            return common::VoidResult::failure(
                ErrorCode::InvalidInput,
                "auth.jwtSecret must be at least 32 characters outside development");
        }
        if (storage.secureLinkSecret.empty()) {
            return common::VoidResult::failure(
                ErrorCode::InvalidInput,
                "storage.secureLinkSecret is required outside development");
        }
        // A length check alone lets the placeholders through: the one in docker-compose.yml is
        // thirty-eight characters long and committed to a public repository, so a deployment
        // that forgets its .env signs tokens with a secret anybody can read — and boots
        // reporting nothing wrong. Refusing them by name is what turns that into a start-up
        // failure with a sentence saying which variable to set.
        if (isDevelopmentPlaceholder(auth.jwtSecret) ||
            isDevelopmentPlaceholder(storage.secureLinkSecret)) {
            return common::VoidResult::failure(
                ErrorCode::InvalidInput,
                "auth.jwtSecret and storage.secureLinkSecret still hold the development "
                "placeholders this repository ships: set JWT_SECRET and FILE_SECURE_LINK_SECRET "
                "to secrets of your own outside development");
        }
        if (database.password.empty()) {
            return common::VoidResult::failure(ErrorCode::InvalidInput,
                                               "database.password is required outside development");
        }
    }

    return common::VoidResult::success();
}

std::filesystem::path resolveConfigPath(const std::string& explicitPath,
                                        const common::EnvLookup& lookup) {
    if (!explicitPath.empty()) {
        return {explicitPath};
    }
    if (auto fromEnv = lookup("LAUNCHER_CONFIG"); fromEnv.has_value() && !fromEnv->empty()) {
        return {*fromEnv};
    }
    const auto environment = lookup("LAUNCHER_ENV").value_or("development");
    return std::filesystem::path("config") / ("config." + environment + ".json");
}

} // namespace launcher::app
