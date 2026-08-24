#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/Hash.h"
#include "migrations/MigrationRunner.h"
#include "support/TemporaryDirectory.h"

namespace {

using launcher::common::ErrorCode;
using launcher::common::sha256Hex;
using launcher::migrations::AppliedMigration;
using launcher::migrations::discoverMigrations;
using launcher::migrations::MigrationFile;
using launcher::migrations::planMigrations;
using launcher::testing::TemporaryDirectory;

MigrationFile makeMigration(std::string version, std::string name, std::string sql) {
    MigrationFile migration;
    migration.version = std::move(version);
    migration.name = std::move(name);
    migration.sql = std::move(sql);
    migration.checksum = sha256Hex(migration.sql);
    return migration;
}

// --------------------------------------------------------------------------
// discoverMigrations
// --------------------------------------------------------------------------

TEST(MigrationDiscoveryTest, ReadsFilesInVersionOrderRegardlessOfDirectoryOrder) {
    const TemporaryDirectory directory("launcher-migrations");
    directory.writeFile("0010_third.sql", "SELECT 3;");
    directory.writeFile("0002_second.sql", "SELECT 2;");
    directory.writeFile("0001_first.sql", "SELECT 1;");

    const auto result = discoverMigrations(directory.path());

    ASSERT_TRUE(result.ok()) << result.error().detail;
    const auto& migrations = result.value();
    ASSERT_EQ(migrations.size(), 3U);
    EXPECT_EQ(migrations[0].version, "0001");
    EXPECT_EQ(migrations[1].version, "0002");
    EXPECT_EQ(migrations[2].version, "0010");
    EXPECT_EQ(migrations[0].name, "first");
    EXPECT_EQ(migrations[0].sql, "SELECT 1;");
}

TEST(MigrationDiscoveryTest, ChecksumIsTheSha256OfTheFileContents) {
    const TemporaryDirectory directory("launcher-migrations");
    directory.writeFile("0001_first.sql", "SELECT 1;");

    const auto result = discoverMigrations(directory.path());

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().at(0).checksum, sha256Hex("SELECT 1;"));
}

TEST(MigrationDiscoveryTest, IgnoresNonSqlFiles) {
    const TemporaryDirectory directory("launcher-migrations");
    directory.writeFile("0001_first.sql", "SELECT 1;");
    directory.writeFile("README.md", "notes");
    directory.writeFile("0002_second.sql.bak", "SELECT 2;");

    const auto result = discoverMigrations(directory.path());

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_EQ(result.value().size(), 1U);
}

// A silently skipped migration is far worse than a loud failure at startup.
TEST(MigrationDiscoveryTest, RejectsAFileThatDoesNotFollowTheNamingPattern) {
    const TemporaryDirectory directory("launcher-migrations");
    directory.writeFile("0001_first.sql", "SELECT 1;");
    directory.writeFile("add-index.sql", "SELECT 2;");

    const auto result = discoverMigrations(directory.path());

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
}

TEST(MigrationDiscoveryTest, RejectsADuplicateVersionNumber) {
    const TemporaryDirectory directory("launcher-migrations");
    directory.writeFile("0001_first.sql", "SELECT 1;");
    directory.writeFile("0001_also_first.sql", "SELECT 2;");

    const auto result = discoverMigrations(directory.path());

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::Conflict);
}

TEST(MigrationDiscoveryTest, AnEmptyDirectoryYieldsNoMigrations) {
    const TemporaryDirectory directory("launcher-migrations");

    const auto result = discoverMigrations(directory.path());

    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.value().empty());
}

TEST(MigrationDiscoveryTest, AMissingDirectoryIsAnError) {
    const TemporaryDirectory directory("launcher-migrations");

    const auto result = discoverMigrations(directory.path() / "nope");

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::NotFound);
}

// --------------------------------------------------------------------------
// planMigrations
// --------------------------------------------------------------------------

TEST(MigrationPlanTest, EverythingIsPendingOnAFreshDatabase) {
    const std::vector<MigrationFile> discovered{makeMigration("0001", "first", "SELECT 1;"),
                                                makeMigration("0002", "second", "SELECT 2;")};

    const auto result = planMigrations(discovered, {});

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_EQ(result.value().pending.size(), 2U);
    EXPECT_EQ(result.value().alreadyApplied, 0U);
}

TEST(MigrationPlanTest, NothingIsPendingWhenTheDatabaseIsUpToDate) {
    const auto first = makeMigration("0001", "first", "SELECT 1;");
    const std::vector<AppliedMigration> applied{{first.version, first.checksum}};

    const auto result = planMigrations({first}, applied);

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_TRUE(result.value().pending.empty());
    EXPECT_EQ(result.value().alreadyApplied, 1U);
}

TEST(MigrationPlanTest, OnlyTheUnappliedTailIsPending) {
    const auto first = makeMigration("0001", "first", "SELECT 1;");
    const auto second = makeMigration("0002", "second", "SELECT 2;");
    const std::vector<AppliedMigration> applied{{first.version, first.checksum}};

    const auto result = planMigrations({first, second}, applied);

    ASSERT_TRUE(result.ok()) << result.error().detail;
    ASSERT_EQ(result.value().pending.size(), 1U);
    EXPECT_EQ(result.value().pending[0].version, "0002");
    EXPECT_EQ(result.value().alreadyApplied, 1U);
}

// Migrations are immutable once merged: editing one that already ran would leave every
// existing deployment on a schema nobody can reproduce.
TEST(MigrationPlanTest, RefusesToRunWhenAnAppliedMigrationHasBeenEdited) {
    const auto edited = makeMigration("0001", "first", "SELECT 1; -- edited after the fact");
    const std::vector<AppliedMigration> applied{{"0001", sha256Hex("SELECT 1;")}};

    const auto result = planMigrations({edited}, applied);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::Conflict);
    EXPECT_NE(result.error().detail.find("has changed"), std::string::npos);
}

TEST(MigrationPlanTest, RefusesToRunWhenAnAppliedMigrationIsMissingFromDisk) {
    const auto first = makeMigration("0001", "first", "SELECT 1;");
    const std::vector<AppliedMigration> applied{{first.version, first.checksum},
                                                {"0002", sha256Hex("SELECT 2;")}};

    const auto result = planMigrations({first}, applied);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::Conflict);
}

// Two branches each adding "0003" and merging would otherwise apply out of order on one
// deployment and in order on another.
TEST(MigrationPlanTest, RefusesAMigrationInsertedBehindTheCurrentHead) {
    const auto inserted = makeMigration("0002", "inserted_later", "SELECT 2;");
    const auto head = makeMigration("0003", "head", "SELECT 3;");
    const std::vector<AppliedMigration> applied{{head.version, head.checksum}};

    const auto result = planMigrations({inserted, head}, applied);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::Conflict);
    EXPECT_NE(result.error().detail.find("renumber"), std::string::npos);
}

TEST(MigrationPlanTest, AnEmptyDirectoryAgainstAnEmptyDatabaseIsAValidNoOp) {
    const auto result = planMigrations({}, {});

    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.value().pending.empty());
    EXPECT_EQ(result.value().alreadyApplied, 0U);
}

} // namespace
