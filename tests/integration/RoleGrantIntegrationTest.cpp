#include <gtest/gtest.h>

#include <atomic>
#include <string>

#include "app/Config.h"
#include "app/RoleGrant.h"
#include "integration/AppHarness.h"

namespace {

using launcher::app::AppConfig;
using launcher::app::grantRoleFromCommandLine;
using launcher::app::RoleGrantOutcome;
using launcher::testing::AppHarness;

AppHarness& harness() {
    return *AppHarness::current();
}

std::string uniqueEmail(const std::string& prefix) {
    static std::atomic<int> counter{0};
    return prefix + std::to_string(counter.fetch_add(1)) + "@example.test";
}

/// A configuration pointing at the harness database. The grant opens its own libpq connection
/// rather than borrowing Drogon's, exactly as the real command does with no server running.
AppConfig configForTestDatabase() {
    AppConfig config;
    const auto& settings = harness().database();
    config.database.name = settings.name();

    // The connection string the command builds comes from the same fields a deployment sets,
    // so it is rebuilt here from the environment the harness itself connected with.
    const auto* host = std::getenv("LAUNCHER_TEST_DB_HOST");
    const auto* user = std::getenv("LAUNCHER_TEST_DB_USER");
    const auto* password = std::getenv("LAUNCHER_TEST_DB_PASSWORD");
    config.database.host = host == nullptr ? "" : host;
    config.database.user = user == nullptr ? "postgres" : user;
    config.database.password = password == nullptr ? "" : password;
    return config;
}

int auditRowsFor(const std::string& email) {
    return harness().database().scalarInt(
        "SELECT count(*) FROM audit_log a JOIN users u ON u.id::text = a.entity_id "
        "WHERE u.email = '" +
        email + "' AND a.action = 'user.role.granted' AND a.actor_user_id IS NULL");
}

} // namespace

TEST(RoleGrantTest, GrantsARoleToAnExistingAccount) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail("grant");
    harness().createVerifiedSession(email);

    const auto result = grantRoleFromCommandLine(configForTestDatabase(), email, "admin");

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_EQ(result.value(), RoleGrantOutcome::Granted);

    EXPECT_EQ(harness().database().scalarInt(
                  "SELECT count(*) FROM user_roles ur JOIN users u ON u.id = ur.user_id "
                  "JOIN roles r ON r.id = ur.role_id WHERE u.email = '" +
                  email + "' AND r.key = 'admin'"),
              1);
}

TEST(RoleGrantTest, RecordsTheGrantWithNoActor) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail("grant-audit");
    harness().createVerifiedSession(email);

    ASSERT_TRUE(grantRoleFromCommandLine(configForTestDatabase(), email, "dev").ok());

    // A command line is not a user, so `actor_user_id` is null. The row still exists, and its
    // metadata says how it happened — otherwise a later reader is left wondering who the
    // missing actor was.
    EXPECT_EQ(auditRowsFor(email), 1);
    EXPECT_EQ(harness().database().scalar(
                  "SELECT metadata->>'via' FROM audit_log WHERE actor_user_id IS NULL "
                  "ORDER BY id DESC LIMIT 1"),
              "command-line");
}

TEST(RoleGrantTest, RunningItTwiceIsNotAFailureAndRecordsOnce) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail("grant-twice");
    harness().createVerifiedSession(email);
    const auto config = configForTestDatabase();

    EXPECT_EQ(grantRoleFromCommandLine(config, email, "dev").value(), RoleGrantOutcome::Granted);

    // Running it again is how somebody checks, and the end state is what was asked for either
    // way. The audit arm selects from the inserting arm, so the second run records nothing.
    EXPECT_EQ(grantRoleFromCommandLine(config, email, "dev").value(),
              RoleGrantOutcome::AlreadyHeld);
    EXPECT_EQ(auditRowsFor(email), 1);
}

TEST(RoleGrantTest, ReportsAnUnknownAccount) {
    LAUNCHER_REQUIRE_DATABASE();

    const auto result =
        grantRoleFromCommandLine(configForTestDatabase(), "nobody@example.test", "admin");

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_EQ(result.value(), RoleGrantOutcome::NoSuchUser);
}

TEST(RoleGrantTest, ReportsAnUnknownRole) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail("grant-badrole");
    harness().createVerifiedSession(email);

    const auto result = grantRoleFromCommandLine(configForTestDatabase(), email, "wizard");

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_EQ(result.value(), RoleGrantOutcome::NoSuchRole);
}

TEST(RoleGrantTest, TheGrantedAdministratorCanThenSignInToTheAdminSurface) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto email = uniqueEmail("grant-bootstrap");
    harness().createVerifiedSession(email);

    // The whole point of the command, end to end: nobody could reach the admin surface, and
    // afterwards somebody can.
    Json::Value credentials;
    credentials["email"] = email;
    credentials["password"] = "correct horse battery staple";

    EXPECT_EQ(harness().adminPostJson("/admin/api/auth/login", credentials)->statusCode(),
              drogon::k403Forbidden);

    ASSERT_TRUE(grantRoleFromCommandLine(configForTestDatabase(), email, "admin").ok());

    EXPECT_EQ(harness().adminPostJson("/admin/api/auth/login", credentials)->statusCode(),
              drogon::k200OK);
}
