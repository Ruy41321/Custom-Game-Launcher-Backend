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

// What separates the administrative routes from the public ones is the listener a request
// arrived on. Collapse the two onto one port and that check passes for everybody, which turns
// user management into a public endpoint — so the configuration is refused rather than served.
TEST(ConfigTest, RejectsAnAdminPortEqualToThePublicOne) {
    constexpr const char* document = R"({
      "environment": "development",
      "server": { "port": 8080, "adminPort": 8080, "adminEnabled": true },
      "database": { "name": "launcher", "user": "launcher" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("adminPort"), std::string::npos);
}

TEST(ConfigTest, ToleratesAnAdminPortEqualToThePublicOneWhileDisabled) {
    // Nothing is listening, so nothing is exposed. Refusing here would make the default
    // document unloadable the moment somebody set both ports to the same value in a template.
    constexpr const char* document = R"({
      "environment": "development",
      "server": { "port": 8080, "adminPort": 8080, "adminEnabled": false },
      "database": { "name": "launcher", "user": "launcher" }
    })";

    EXPECT_TRUE(AppConfig::parse(document, lookupFrom({})).ok());
}

TEST(ConfigTest, RejectsAZeroAdminPortWhileEnabled) {
    constexpr const char* document = R"({
      "environment": "development",
      "server": { "port": 8080, "adminPort": 0, "adminEnabled": true },
      "database": { "name": "launcher", "user": "launcher" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("adminPort"), std::string::npos);
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
      "mail": { "transport": "smtp", "host": "smtp.example.com",
                "fromAddress": "no-reply@example.com" },
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
      "mail": { "transport": "smtp", "host": "smtp.example.com",
                "fromAddress": "no-reply@example.com" },
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
      "mail": { "transport": "smtp", "host": "smtp.example.com",
                "fromAddress": "no-reply@example.com" },
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
      "mail": { "transport": "smtp", "host": "smtp.example.com",
                "fromAddress": "no-reply@example.com" },
      "database": { "name": "launcher", "user": "launcher", "password": "pw" },
      "storage": { "secureLinkSecret": "link-secret" },
      "auth": { "jwtSecret": "0123456789abcdef0123456789abcdef" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_TRUE(std::move(result).value().isProduction());
}

TEST(ConfigTest, ProductionRefusesTheDevelopmentPlaceholderSecretsByName) {
    // The length check alone let these through: the placeholder in docker-compose.yml is
    // thirty-eight characters long and committed to a public repository, so a deployment that
    // forgot its .env signed every token with a secret anybody could read — and started up
    // reporting nothing wrong at all.
    constexpr const char* document = R"({
      "environment": "production",
      "mail": { "transport": "smtp", "host": "smtp.example.com",
                "fromAddress": "no-reply@example.com" },
      "database": { "name": "launcher", "user": "launcher", "password": "pw" },
      "storage": { "secureLinkSecret": "link-secret" },
      "auth": { "jwtSecret": "dev-insecure-jwt-secret-do-not-deploy" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("placeholder"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Mail. A deployment that cannot deliver a verification link is one where nobody can finish
// registering, so the refusals below happen at start-up rather than at the first registration.
// ---------------------------------------------------------------------------

TEST(ConfigTest, SmtpWithoutARelayIsRefusedEvenInDevelopment) {
    constexpr const char* document = R"({
      "environment": "development",
      "database": { "name": "launcher", "user": "launcher" },
      "mail": { "transport": "smtp", "fromAddress": "no-reply@example.com" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("mail.host"), std::string::npos);
}

// A document that says nothing about mail gets the transport that cannot leak anything and
// cannot fail to connect — and, outside development, is refused for exactly that reason.
TEST(ConfigTest, ADocumentWithNoMailSectionGetsTheLogTransport) {
    const auto result = AppConfig::parse(MINIMAL_DEVELOPMENT_CONFIG, lookupFrom({}));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_EQ(std::move(result).value().mail.transport, launcher::app::MailTransport::Log);
}

TEST(ConfigTest, ProductionRefusesADocumentThatSaysNothingAboutMailAtAll) {
    constexpr const char* document = R"({
      "environment": "production",
      "database": { "name": "launcher", "user": "launcher", "password": "pw" },
      "storage": { "secureLinkSecret": "link-secret" },
      "auth": { "jwtSecret": "0123456789abcdef0123456789abcdef" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("MAIL_TRANSPORT"), std::string::npos);
}

// The log transport writes the body of the message, and the body of a reset message is a live
// credential. Selecting it on a deployed environment would be filing credentials into a log.
TEST(ConfigTest, ProductionRefusesTheLogTransport) {
    constexpr const char* document = R"({
      "environment": "production",
      "database": { "name": "launcher", "user": "launcher", "password": "pw" },
      "storage": { "secureLinkSecret": "link-secret" },
      "auth": { "jwtSecret": "0123456789abcdef0123456789abcdef" },
      "mail": { "transport": "log" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("MAIL_TRANSPORT"), std::string::npos);
}

TEST(ConfigTest, ProductionRefusesARequiredVerificationNothingCanDeliver) {
    constexpr const char* document = R"({
      "environment": "production",
      "database": { "name": "launcher", "user": "launcher", "password": "pw" },
      "storage": { "secureLinkSecret": "link-secret" },
      "auth": { "jwtSecret": "0123456789abcdef0123456789abcdef", "requireVerifiedEmail": true },
      "mail": { "transport": "none" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("requireVerifiedEmail"), std::string::npos);
}

// Turning mail off is a legitimate configuration, and this is the shape it has to take.
TEST(ConfigTest, ProductionAcceptsNoMailAtAllWhenNothingRequiresAVerifiedAddress) {
    constexpr const char* document = R"({
      "environment": "production",
      "database": { "name": "launcher", "user": "launcher", "password": "pw" },
      "storage": { "secureLinkSecret": "link-secret" },
      "auth": { "jwtSecret": "0123456789abcdef0123456789abcdef", "requireVerifiedEmail": false },
      "mail": { "transport": "none" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_FALSE(std::move(result).value().mail.enabled());
}

TEST(ConfigTest, RejectsATransportNobodyImplements) {
    constexpr const char* document = R"({
      "environment": "development",
      "database": { "name": "launcher", "user": "launcher" },
      "mail": { "transport": "carrier-pigeon" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("carrier-pigeon"), std::string::npos);
}

TEST(ConfigTest, RejectsALinkBaseUrlThatIsNotThere) {
    constexpr const char* document = R"({
      "environment": "development",
      "database": { "name": "launcher", "user": "launcher" },
      "mail": { "transport": "log", "linkBaseUrl": "" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("linkBaseUrl"), std::string::npos);
}

TEST(ConfigTest, ProductionRefusesThePlaceholderSigningSecretToo) {
    constexpr const char* document = R"({
      "environment": "production",
      "mail": { "transport": "smtp", "host": "smtp.example.com",
                "fromAddress": "no-reply@example.com" },
      "database": { "name": "launcher", "user": "launcher", "password": "pw" },
      "storage": { "secureLinkSecret": "dev-insecure-secure-link-secret" },
      "auth": { "jwtSecret": "0123456789abcdef0123456789abcdef" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("placeholder"), std::string::npos);
}

TEST(ConfigTest, DevelopmentKeepsBootingOnThePlaceholders) {
    // Which is what they are for: the stack comes up with no setup at all, and only a
    // deployment is asked to have made a decision.
    constexpr const char* document = R"({
      "environment": "development",
      "database": { "name": "launcher", "user": "launcher" },
      "storage": { "secureLinkSecret": "dev-insecure-secure-link-secret" },
      "auth": { "jwtSecret": "dev-insecure-jwt-secret-do-not-deploy" }
    })";

    EXPECT_TRUE(AppConfig::parse(document, lookupFrom({})).ok());
}

TEST(ConfigTest, ReadsTheHardeningKnobsAndTheirDefaults) {
    constexpr const char* document = R"({
      "environment": "development",
      "server": {
        "port": 8080,
        "maxDocumentBytes": 1048576,
        "maxAnonymousBodyBytes": 4096,
        "trustedProxies": ["172.16.0.0/12", "10.0.0.5"]
      },
      "security": { "hsts": true, "hstsMaxAgeSeconds": 120 },
      "rateLimit": { "accountRequests": 42, "accountWindowSeconds": 30 },
      "database": { "name": "launcher", "user": "launcher" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    const auto config = std::move(result).value();

    EXPECT_EQ(config.server.maxDocumentBytes, 1048576);
    EXPECT_EQ(config.server.maxAnonymousBodyBytes, 4096);
    ASSERT_EQ(config.server.trustedProxies.size(), 2U);
    EXPECT_EQ(config.server.trustedProxies[0], "172.16.0.0/12");
    EXPECT_TRUE(config.security.hsts);
    EXPECT_EQ(config.security.hstsMaxAgeSeconds, 120U);
    EXPECT_EQ(config.rateLimit.accountRequests, 42U);
    EXPECT_EQ(config.rateLimit.accountWindowSeconds, 30U);
}

TEST(ConfigTest, DefaultsTrustNoProxyAndSayNothingAboutTransportSecurity) {
    const auto result = AppConfig::parse(MINIMAL_DEVELOPMENT_CONFIG, lookupFrom({}));

    ASSERT_TRUE(result.ok()) << result.error().detail;
    const auto config = std::move(result).value();

    EXPECT_TRUE(config.server.trustedProxies.empty());
    EXPECT_FALSE(config.security.hsts);
    EXPECT_EQ(config.rateLimit.accountRequests, 600U);
}

TEST(ConfigTest, RejectsAnAnonymousCapLargerThanTheDocumentOne) {
    constexpr const char* document = R"({
      "environment": "development",
      "server": { "port": 8080, "maxDocumentBytes": 4096, "maxAnonymousBodyBytes": 8192 },
      "database": { "name": "launcher", "user": "launcher" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("maxAnonymousBodyBytes"), std::string::npos);
}

TEST(ConfigTest, RejectsARateLimitOfZero) {
    // A zero bucket refuses everybody, which reads as an outage rather than as a throttle.
    // Turning a limit off is not a supported configuration; widening it is.
    constexpr const char* document = R"({
      "environment": "development",
      "server": { "port": 8080 },
      "rateLimit": { "accountRequests": 0 },
      "database": { "name": "launcher", "user": "launcher" }
    })";

    const auto result = AppConfig::parse(document, lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().detail.find("rateLimit"), std::string::npos);
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
