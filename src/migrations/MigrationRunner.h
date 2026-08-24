#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/Result.h"

namespace launcher::migrations {

/// A migration file on disk. `version` is the numeric prefix, which orders execution and
/// is the primary key in `schema_migrations`.
struct MigrationFile {
    std::string version;
    std::string name;
    std::filesystem::path path;
    std::string sql;
    std::string checksum; ///< lowercase hex SHA-256 of `sql`
};

/// A row of `schema_migrations`, i.e. what the database believes it has already run.
struct AppliedMigration {
    std::string version;
    std::string checksum;
};

struct MigrationPlan {
    std::vector<MigrationFile> pending;
    std::size_t alreadyApplied{0};
};

/// Reads `NNNN_name.sql` files from `directory`, hashes them, and returns them in version
/// order. Files that do not match the naming pattern are an error rather than a silent skip.
common::Result<std::vector<MigrationFile>>
discoverMigrations(const std::filesystem::path& directory);

/// Decides what still has to run. Refuses to proceed when history has been tampered with:
/// an applied migration whose file changed or disappeared, or a new migration inserted
/// behind ones that already ran. Pure function — unit tested without a database.
common::Result<MigrationPlan> planMigrations(const std::vector<MigrationFile>& discovered,
                                             const std::vector<AppliedMigration>& applied);

/// Applies pending migrations over a dedicated libpq connection.
///
/// This deliberately does not go through Drogon's DbClient. When libpq is new enough to
/// support pipeline mode, Drogon compiles the batched PostgreSQL backend, which sends every
/// statement through the extended query protocol — and that protocol rejects the
/// multi-statement scripts a migration file consists of ("cannot insert multiple commands
/// into a prepared statement"). libpq's simple query protocol accepts them and, as a bonus,
/// gives exactly the all-or-nothing semantics a migration needs.
class MigrationRunner {
  public:
    struct Summary {
        std::size_t applied{0};
        std::size_t alreadyApplied{0};
    };

    MigrationRunner(std::string connectionString, std::filesystem::path directory);

    /// Applies every pending migration, each inside its own transaction together with its
    /// `schema_migrations` bookkeeping row, so a failure leaves no partial history.
    common::Result<Summary> run();

  private:
    std::string connectionString_;
    std::filesystem::path directory_;
};

} // namespace launcher::migrations
