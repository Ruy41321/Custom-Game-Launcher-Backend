#include <gtest/gtest.h>

#include <map>
#include <string>

#include "app/Config.h"

namespace {

using launcher::app::AppConfig;
using launcher::app::resolveConfigPath;
using launcher::common::EnvLookup;
using launcher::common::ErrorCode;

EnvLookup lookupFrom(std::map<std::string, std::string> values) {
    return [values = std::move(values)](const std::string& name) -> std::optional<std::string> {
        const auto found = values.find(name);
        if (found == values.end()) {
            return std::nullopt;
        }
        return found->second;
    };
}

constexpr const char* MINIMAL_DEVELOPMENT_CONFIG = R"({
  "environment": "development",
  "server": { "port": 8080 },
  "database": { "name": "launcher", "user": "launcher" }
})";

TEST(ConfigTest, ParsesAMinimalDocumentAndFillsInDefaults) {
    const auto result = AppConfig::parse(MINIMAL_DEVELOPMENT_CONFIG, lookupFrom({}));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    const auto config = std::move(result).value();

    EXPECT_EQ(config.environment, "development");
    EXPECT_EQ(config.server.port, 8080);
    EXPECT_EQ(config.server.listenAddress, "0.0.0.0");
    EXPECT_EQ(config.server.adminListenAddress, "127.0.0.1");
    EXPECT_FALSE(config.server.adminEnabled);
    EXPECT_EQ(config.database.host, "localhost");
    EXPECT_EQ(config.auth.accessTokenTtlSeconds, 900U);
    EXPECT_DOUBLE_EQ(config.updates.fullDownloadThresholdRatio, 0.7);
    EXPECT_EQ(config.uploads.defaultQuotaBytes, 5368709120LL);
}

TEST(ConfigTest, ExpandsEnvironmentPlaceholdersIncludingNumericOnes) {
    constexpr const char* document = R"({
      "environment": "development",
      "server": { "port": ${SERVER_PORT:-8080} },
      "database": { "name": "launcher", "user": "launcher", "password": "${DB_PASSWORD}" }
    })";

    const auto result = AppConfig::parse(
        document, lookupFrom({{"SERVER_PORT", "9999"}, {"DB_PASSWORD", "s3cret"}}));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    const auto config = std::move(result).value();

    EXPECT_EQ(config.server.port, 9999);
    EXPECT_EQ(config.database.password, "s3cret");
}

TEST(ConfigTest, RejectsMalformedJson) {
    const auto result = AppConfig::parse("{ not json", lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
}

TEST(ConfigTest, RejectsANonObjectRoot) {
    const auto result = AppConfig::parse("[1, 2, 3]", lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
}

TEST(ConfigTest, RejectsAnOutOfRangeDownloadThreshold) {
    constexpr const char* document = R"({
      "environment": "development",
      "database": { "name": "launcher", "user": "launcher" },
      "updates": { "fullDownloadThresholdRatio": 1.5 }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("fullDownloadThresholdRatio"), std::string::npos);
}

TEST(ConfigTest, RejectsANonPositiveUploadQuota) {
    constexpr const char* document = R"({
      "environment": "development",
      "database": { "name": "launcher", "user": "launcher" },
      "uploads": { "defaultQuotaBytes": 0 }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("defaultQuotaBytes"), std::string::npos);
}

// Development boots with no setup; anything deployed must not.
TEST(ConfigTest, DevelopmentToleratesBlankSecrets) {
    const auto result = AppConfig::parse(MINIMAL_DEVELOPMENT_CONFIG, lookupFrom({}));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_TRUE(std::move(result).value().auth.jwtSecret.empty());
}

TEST(ConfigTest, ProductionRejectsAWeakJwtSecret) {
    constexpr const char* document = R"({
      "environment": "production",
      "database": { "name": "launcher", "user": "launcher", "password": "pw" },
      "storage": { "secureLinkSecret": "link-secret" },
      "auth": { "jwtSecret": "too-short" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("jwtSecret"), std::string::npos);
}

TEST(ConfigTest, ProductionRejectsAMissingSecureLinkSecret) {
    constexpr const char* document = R"({
      "environment": "production",
      "database": { "name": "launcher", "user": "launcher", "password": "pw" },
      "auth": { "jwtSecret": "0123456789abcdef0123456789abcdef" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("secureLinkSecret"), std::string::npos);
}

TEST(ConfigTest, ProductionRejectsABlankDatabasePassword) {
    constexpr const char* document = R"({
      "environment": "production",
      "database": { "name": "launcher", "user": "launcher" },
      "storage": { "secureLinkSecret": "link-secret" },
      "auth": { "jwtSecret": "0123456789abcdef0123456789abcdef" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("database.password"), std::string::npos);
}

TEST(ConfigTest, ProductionAcceptsAFullyConfiguredDocument) {
    constexpr const char* document = R"({
      "environment": "production",
      "database": { "name": "launcher", "user": "launcher", "password": "pw" },
      "storage": { "secureLinkSecret": "link-secret" },
      "auth": { "jwtSecret": "0123456789abcdef0123456789abcdef" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_TRUE(std::move(result).value().isProduction());
}

TEST(ConfigTest, DatabaseConnectionStringCarriesEveryField) {
    launcher::app::DatabaseConfig database;
    database.host = "db";
    database.port = 6543;
    database.name = "launcher";
    database.user = "app";
    database.password = "pw";

    const auto connection = database.connectionString();

    EXPECT_NE(connection.find("host=db"), std::string::npos);
    EXPECT_NE(connection.find("port=6543"), std::string::npos);
    EXPECT_NE(connection.find("dbname=launcher"), std::string::npos);
    EXPECT_NE(connection.find("user=app"), std::string::npos);
    EXPECT_NE(connection.find("password=pw"), std::string::npos);
}

TEST(ConfigTest, ConfigPathPrefersTheExplicitArgument) {
    const auto path =
        resolveConfigPath("/etc/launcher.json", lookupFrom({{"LAUNCHER_CONFIG", "/ignored.json"}}));

    EXPECT_EQ(path.string(), "/etc/launcher.json");
}

TEST(ConfigTest, ConfigPathFallsBackToTheEnvironmentVariable) {
    const auto path =
        resolveConfigPath("", lookupFrom({{"LAUNCHER_CONFIG", "/etc/from-env.json"}}));

    EXPECT_EQ(path.string(), "/etc/from-env.json");
}

TEST(ConfigTest, ConfigPathDerivesFromTheEnvironmentName) {
    const auto path = resolveConfigPath("", lookupFrom({{"LAUNCHER_ENV", "staging"}}));

    EXPECT_EQ(path.filename().string(), "config.staging.json");
}

TEST(ConfigTest, ConfigPathDefaultsToDevelopment) {
    const auto path = resolveConfigPath("", lookupFrom({}));

    EXPECT_EQ(path.filename().string(), "config.development.json");
}

} // namespace
