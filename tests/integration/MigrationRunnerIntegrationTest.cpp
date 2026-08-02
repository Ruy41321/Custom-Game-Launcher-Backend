#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "integration/TestDatabase.h"
#include "migrations/MigrationRunner.h"
#include "support/TemporaryDirectory.h"

#ifndef LAUNCHER_MIGRATIONS_DIR
#define LAUNCHER_MIGRATIONS_DIR "migrations"
#endif

namespace {

using launcher::migrations::MigrationRunner;
using launcher::testing::TemporaryDirectory;
using launcher::testing::TestDatabase;

class MigrationRunnerIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        database_ = TestDatabase::createOrNull();
        if (!database_) {
            GTEST_SKIP() << "LAUNCHER_TEST_DB_HOST is not set; skipping database integration test";
        }
    }

    MigrationRunner repositoryRunner() {
        return MigrationRunner(database_->connectionString(), LAUNCHER_MIGRATIONS_DIR);
    }

    std::unique_ptr<TestDatabase> database_;
};

TEST_F(MigrationRunnerIntegrationTest, AppliesTheRepositoryMigrationsToAnEmptyDatabase) {
    auto runner = repositoryRunner();

    const auto result = runner.run();

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_GE(result.value().applied, 1U);
    EXPECT_EQ(result.value().alreadyApplied, 0U);
}

TEST_F(MigrationRunnerIntegrationTest, CreatesTheExpectedTables) {
    auto runner = repositoryRunner();
    ASSERT_TRUE(runner.run().ok());

    for (const auto* table : {"users",
                              "roles",
                              "permissions",
                              "role_permissions",
                              "user_roles",
                              "refresh_tokens",
                              "user_tokens",
                              "games",
                              "game_media",
                              "game_versions",
                              "builds",
                              "blobs",
                              "build_files",
                              "patch_notes",
                              "user_games",
                              "download_events",
                              "audit_log",
                              "account_deletion_requests",
                              "schema_migrations"}) {
        EXPECT_TRUE(database_->scalarBool(std::string("SELECT to_regclass('public.") + table +
                                          "') IS NOT NULL"))
            << "missing table: " << table;
    }
}

TEST_F(MigrationRunnerIntegrationTest, SeedsTheSystemRolesAndTheirPermissions) {
    auto runner = repositoryRunner();
    ASSERT_TRUE(runner.run().ok());

    EXPECT_EQ(
        database_->scalarInt("SELECT count(*) FROM roles WHERE key IN ('player','dev','admin')"),
        3);
    EXPECT_TRUE(database_->scalarBool("SELECT bool_and(is_system) FROM roles"));

    // A dev can do everything a player can, and strictly more.
    const auto playerPermissions = database_->scalarInt(
        "SELECT count(*) FROM role_permissions rp JOIN roles r ON r.id = rp.role_id "
        "WHERE r.key = 'player'");
    const auto devPermissions = database_->scalarInt(
        "SELECT count(*) FROM role_permissions rp JOIN roles r ON r.id = rp.role_id "
        "WHERE r.key = 'dev'");
    const auto adminPermissions = database_->scalarInt(
        "SELECT count(*) FROM role_permissions rp JOIN roles r ON r.id = rp.role_id "
        "WHERE r.key = 'admin'");
    const auto allPermissions = database_->scalarInt("SELECT count(*) FROM permissions");

    EXPECT_GT(playerPermissions, 0);
    EXPECT_GT(devPermissions, playerPermissions);
    EXPECT_EQ(adminPermissions, allPermissions);

    EXPECT_EQ(database_->scalarInt("SELECT count(*) FROM ( "
                                   "  SELECT rp.permission_id FROM role_permissions rp "
                                   "  JOIN roles r ON r.id = rp.role_id WHERE r.key = 'player' "
                                   "  EXCEPT "
                                   "  SELECT rp.permission_id FROM role_permissions rp "
                                   "  JOIN roles r ON r.id = rp.role_id WHERE r.key = 'dev' "
                                   ") missing"),
              0);
}

TEST_F(MigrationRunnerIntegrationTest, RunningTwiceIsANoOp) {
    auto runner = repositoryRunner();
    const auto first = runner.run();
    ASSERT_TRUE(first.ok()) << first.error().detail;

    const auto second = runner.run();

    ASSERT_TRUE(second.ok()) << second.error().detail;
    EXPECT_EQ(second.value().applied, 0U);
    EXPECT_EQ(second.value().alreadyApplied, first.value().applied);
}

TEST_F(MigrationRunnerIntegrationTest, RecordsHistoryWithTheFileChecksum) {
    auto runner = repositoryRunner();
    ASSERT_TRUE(runner.run().ok());

    const auto rows =
        database_->query("SELECT version, name, checksum FROM schema_migrations ORDER BY version");

    ASSERT_GE(rows.size(), 1U);
    EXPECT_EQ(rows[0][0], "0001");
    EXPECT_EQ(rows[0][1], "initial_schema");
    EXPECT_EQ(rows[0][2].size(), 64U);
}

// An edited migration must stop the process rather than silently diverge deployments.
TEST_F(MigrationRunnerIntegrationTest, RefusesToRunWhenAnAppliedMigrationWasEdited) {
    const TemporaryDirectory directory("launcher-migrations");
    directory.writeFile("0001_only.sql", "CREATE TABLE widgets (id int primary key);");

    MigrationRunner first(database_->connectionString(), directory.path());
    ASSERT_TRUE(first.run().ok());

    directory.writeFile("0001_only.sql", "CREATE TABLE widgets (id int primary key, extra text);");
    MigrationRunner second(database_->connectionString(), directory.path());

    const auto result = second.run();

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, launcher::common::ErrorCode::Conflict);
}

// A failing statement must leave neither schema changes nor a history row behind.
TEST_F(MigrationRunnerIntegrationTest, AFailedMigrationIsRolledBackEntirely) {
    const TemporaryDirectory directory("launcher-migrations");
    directory.writeFile("0001_broken.sql",
                        "CREATE TABLE gadgets (id int primary key);\n"
                        "THIS IS NOT VALID SQL;\n");

    MigrationRunner runner(database_->connectionString(), directory.path());

    const auto result = runner.run();

    ASSERT_FALSE(result.ok());
    EXPECT_FALSE(database_->scalarBool("SELECT to_regclass('public.gadgets') IS NOT NULL"));
    EXPECT_EQ(
        database_->scalarInt("SELECT count(*) FROM schema_migrations WHERE version = '0001'"), 0);
}

// Defence in depth: the API validates manifest paths too, but a malicious manifest must not
// even be storable.
TEST_F(MigrationRunnerIntegrationTest, RejectsPathTraversalInBuildFiles) {
    auto runner = repositoryRunner();
    ASSERT_TRUE(runner.run().ok());

    database_->exec(
        "INSERT INTO blobs (sha256, size_bytes, storage_key) "
        "VALUES (repeat('a', 64), 10, 'aa/aa/blob')");
    database_->exec(
        "INSERT INTO users (email, password_hash, display_name) "
        "VALUES ('dev@example.com', 'x', 'Dev')");
    database_->exec(
        "INSERT INTO games (slug, title, publisher_user_id) "
        "SELECT 'test-game', 'Test Game', id FROM users LIMIT 1");
    database_->exec(
        "INSERT INTO game_versions (game_id, semver, version_major) "
        "SELECT id, '1.0', 1 FROM games LIMIT 1");
    database_->exec(
        "INSERT INTO builds (game_version_id, platform) "
        "SELECT id, 'windows' FROM game_versions LIMIT 1");

    constexpr const char* INSERT_BUILD_FILE =
        "INSERT INTO build_files (build_id, relative_path, blob_sha256) "
        "SELECT id, $1, repeat('a', 64) FROM builds LIMIT 1";

    for (const auto* dangerous :
         {"../escape.dll", "/etc/passwd", "nested/../../escape", "C:\\x", ""}) {
        EXPECT_FALSE(database_->tryExec(INSERT_BUILD_FILE, {dangerous}))
            << "path should have been rejected: " << dangerous;
    }

    EXPECT_TRUE(database_->tryExec(INSERT_BUILD_FILE, {"bin/game.exe"}));
    EXPECT_EQ(database_->scalarInt("SELECT count(*) FROM build_files"), 1);
}

}  // namespace
