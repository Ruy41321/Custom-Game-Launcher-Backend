#pragma once

#include <drogon/orm/DbClient.h>
#include <libpq-fe.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace launcher::testing {

/// Connection details for the PostgreSQL instance integration tests may use.
/// Populated from the environment:
///
///   LAUNCHER_TEST_DB_HOST      required — absence means "no database, skip"
///   LAUNCHER_TEST_DB_PORT      default 5432
///   LAUNCHER_TEST_DB_USER      default postgres
///   LAUNCHER_TEST_DB_PASSWORD  default empty
///   LAUNCHER_TEST_DB_ADMIN     default postgres (database used to CREATE/DROP)
struct TestDatabaseSettings {
    std::string host;
    uint16_t port{5432};
    std::string user{"postgres"};
    std::string password;
    std::string adminDatabase{"postgres"};

    static std::optional<TestDatabaseSettings> fromEnvironment();

    std::string connectionStringFor(const std::string& database) const;
};

/// A freshly created, empty database that is dropped when the object goes out of scope.
/// Each test gets its own, so tests never see each other's schema or rows.
///
/// Everything the harness itself does — creating the database, asserting on rows, dropping
/// it again — goes through a synchronous libpq connection rather than Drogon. Drogon's
/// DbClient is asynchronous, and destroying one can land on its own connection loop thread,
/// where joining that thread aborts the process with "Resource deadlock avoided". Test
/// setup and teardown must not be able to fail that way.
class TestDatabase {
  public:
    /// Returns nullptr when the environment provides no database; callers should
    /// GTEST_SKIP in that case.
    static std::unique_ptr<TestDatabase> createOrNull();

    ~TestDatabase();

    TestDatabase(const TestDatabase&) = delete;
    TestDatabase& operator=(const TestDatabase&) = delete;
    TestDatabase(TestDatabase&&) = delete;
    TestDatabase& operator=(TestDatabase&&) = delete;

    const std::string& name() const { return name_; }

    std::string connectionString() const { return settings_.connectionStringFor(name_); }

    /// Drogon client for this database, created on first use. Only tests that exercise
    /// application code needing a DbClient should touch this.
    const drogon::orm::DbClientPtr& client();

    /// Runs a statement, throwing std::runtime_error if the database rejects it.
    void exec(const std::string& sql);

    /// Runs a possibly-failing statement and reports whether it succeeded. Used to assert
    /// that a CHECK constraint does its job.
    bool tryExec(const std::string& sql, const std::vector<std::string>& parameters = {});

    /// First column of the first row, as text.
    std::string scalar(const std::string& sql);

    bool scalarBool(const std::string& sql);

    int scalarInt(const std::string& sql);

    std::vector<std::vector<std::string>> query(const std::string& sql);

  private:
    TestDatabase(TestDatabaseSettings settings, std::string name, PGconn* connection);

    TestDatabaseSettings settings_;
    std::string name_;
    PGconn* connection_{nullptr};
    drogon::orm::DbClientPtr client_;  ///< null until client() is called
};

}  // namespace launcher::testing
