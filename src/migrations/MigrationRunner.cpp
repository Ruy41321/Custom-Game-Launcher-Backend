#include "migrations/MigrationRunner.h"

#include <libpq-fe.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "common/Hash.h"
#include "common/Logging.h"

namespace launcher::migrations {
namespace {

using common::ErrorCode;
using common::Result;
using common::VoidResult;

constexpr const char* HISTORY_TABLE_DDL = R"(
CREATE TABLE IF NOT EXISTS schema_migrations (
    version    text        PRIMARY KEY,
    name       text        NOT NULL,
    checksum   text        NOT NULL,
    applied_at timestamptz NOT NULL DEFAULT now()
);
)";

using ConnectionPtr = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using ResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

bool isAllDigits(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
}

/// Splits `0001_initial_schema.sql` into ("0001", "initial_schema").
bool parseFileName(const std::string& stem, std::string& version, std::string& name) {
    const auto separator = stem.find('_');
    if (separator == std::string::npos) {
        return false;
    }
    version = stem.substr(0, separator);
    name = stem.substr(separator + 1);
    return isAllDigits(version) && !name.empty();
}

std::string trimmed(const char* message) {
    std::string text = message == nullptr ? std::string{} : std::string(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

/// Runs a statement (or a whole multi-statement script) through the simple query protocol.
VoidResult exec(PGconn* connection, const std::string& sql) {
    const ResultPtr result(PQexec(connection, sql.c_str()), &PQclear);
    const auto status = PQresultStatus(result.get());
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        return VoidResult::failure(ErrorCode::DependencyFailure,
                                   trimmed(PQerrorMessage(connection)));
    }
    return VoidResult::success();
}

VoidResult
execParams(PGconn* connection, const std::string& sql, const std::vector<std::string>& parameters) {
    std::vector<const char*> values;
    values.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        values.push_back(parameter.c_str());
    }

    const ResultPtr result(PQexecParams(connection,
                                        sql.c_str(),
                                        static_cast<int>(values.size()),
                                        nullptr,
                                        values.data(),
                                        nullptr,
                                        nullptr,
                                        0),
                           &PQclear);

    if (PQresultStatus(result.get()) != PGRES_COMMAND_OK) {
        return VoidResult::failure(ErrorCode::DependencyFailure,
                                   trimmed(PQerrorMessage(connection)));
    }
    return VoidResult::success();
}

Result<std::vector<AppliedMigration>> loadApplied(PGconn* connection) {
    const ResultPtr result(PQexec(connection, "SELECT version, checksum FROM schema_migrations"),
                           &PQclear);

    if (PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
        return Result<std::vector<AppliedMigration>>::failure(
            ErrorCode::DependencyFailure,
            "cannot read schema_migrations: " + trimmed(PQerrorMessage(connection)));
    }

    const int rowCount = PQntuples(result.get());
    std::vector<AppliedMigration> applied;
    applied.reserve(static_cast<std::size_t>(rowCount));
    for (int row = 0; row < rowCount; ++row) {
        applied.push_back(
            AppliedMigration{PQgetvalue(result.get(), row, 0), PQgetvalue(result.get(), row, 1)});
    }
    return Result<std::vector<AppliedMigration>>::success(std::move(applied));
}

} // namespace

Result<std::vector<MigrationFile>> discoverMigrations(const std::filesystem::path& directory) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return Result<std::vector<MigrationFile>>::failure(
            ErrorCode::NotFound, "migrations directory not found: " + directory.string());
    }

    std::vector<MigrationFile> migrations;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".sql") {
            continue;
        }

        MigrationFile migration;
        if (!parseFileName(entry.path().stem().string(), migration.version, migration.name)) {
            return Result<std::vector<MigrationFile>>::failure(
                ErrorCode::InvalidInput,
                "migration file does not follow the NNNN_name.sql pattern: " +
                    entry.path().filename().string());
        }

        std::ifstream stream(entry.path(), std::ios::binary);
        if (!stream) {
            return Result<std::vector<MigrationFile>>::failure(
                ErrorCode::Internal, "cannot read migration: " + entry.path().string());
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();

        migration.path = entry.path();
        migration.sql = buffer.str();
        migration.checksum = common::sha256Hex(migration.sql);
        migrations.push_back(std::move(migration));
    }

    std::sort(migrations.begin(), migrations.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.version < rhs.version;
    });

    for (std::size_t i = 1; i < migrations.size(); ++i) {
        if (migrations[i].version == migrations[i - 1].version) {
            return Result<std::vector<MigrationFile>>::failure(
                ErrorCode::Conflict, "duplicate migration version: " + migrations[i].version);
        }
    }

    return Result<std::vector<MigrationFile>>::success(std::move(migrations));
}

Result<MigrationPlan> planMigrations(const std::vector<MigrationFile>& discovered,
                                     const std::vector<AppliedMigration>& applied) {
    std::unordered_map<std::string, std::string> appliedByVersion;
    appliedByVersion.reserve(applied.size());
    for (const auto& row : applied) {
        appliedByVersion.emplace(row.version, row.checksum);
    }

    std::string highestApplied;
    for (const auto& row : applied) {
        highestApplied = std::max(highestApplied, row.version);
    }

    MigrationPlan plan;
    std::size_t matched = 0;

    for (const auto& migration : discovered) {
        const auto found = appliedByVersion.find(migration.version);
        if (found == appliedByVersion.end()) {
            if (!highestApplied.empty() && migration.version < highestApplied) {
                return Result<MigrationPlan>::failure(
                    ErrorCode::Conflict,
                    "migration " + migration.version +
                        " was added behind already applied migration " + highestApplied +
                        "; renumber it to run after the current head");
            }
            plan.pending.push_back(migration);
            continue;
        }

        ++matched;
        if (found->second != migration.checksum) {
            return Result<MigrationPlan>::failure(
                ErrorCode::Conflict,
                "migration " + migration.version + " (" + migration.name +
                    ") has changed since it was applied; migrations are immutable once "
                    "merged — add a new migration instead");
        }
    }

    if (matched != applied.size()) {
        return Result<MigrationPlan>::failure(
            ErrorCode::Conflict,
            "the database reports migrations that no longer exist on disk; refusing to run");
    }

    plan.alreadyApplied = matched;
    return Result<MigrationPlan>::success(std::move(plan));
}

MigrationRunner::MigrationRunner(std::string connectionString, std::filesystem::path directory)
    : connectionString_(std::move(connectionString)),
      directory_(std::move(directory)) {}

Result<MigrationRunner::Summary> MigrationRunner::run() {
    const ConnectionPtr connection(PQconnectdb(connectionString_.c_str()), &PQfinish);
    if (PQstatus(connection.get()) != CONNECTION_OK) {
        return Result<Summary>::failure(ErrorCode::DependencyFailure,
                                        "cannot connect to the database: " +
                                            trimmed(PQerrorMessage(connection.get())));
    }

    if (auto ready = exec(connection.get(), HISTORY_TABLE_DDL); !ready.ok()) {
        return Result<Summary>::failure(ErrorCode::DependencyFailure,
                                        "cannot create schema_migrations: " + ready.error().detail);
    }

    auto discovered = discoverMigrations(directory_);
    if (!discovered.ok()) {
        return Result<Summary>::failure(discovered.error());
    }

    auto applied = loadApplied(connection.get());
    if (!applied.ok()) {
        return Result<Summary>::failure(applied.error());
    }

    auto plan = planMigrations(std::move(discovered).value(), std::move(applied).value());
    if (!plan.ok()) {
        return Result<Summary>::failure(plan.error());
    }

    const auto migrationPlan = std::move(plan).value();
    Summary summary{0, migrationPlan.alreadyApplied};

    for (const auto& migration : migrationPlan.pending) {
        spdlog::info("applying migration {}_{}", migration.version, migration.name);

        if (auto begun = exec(connection.get(), "BEGIN"); !begun.ok()) {
            return Result<Summary>::failure(begun.error());
        }

        auto applyResult = exec(connection.get(), migration.sql);
        if (applyResult.ok()) {
            applyResult = execParams(
                connection.get(),
                "INSERT INTO schema_migrations (version, name, checksum) VALUES ($1, $2, $3)",
                {migration.version, migration.name, migration.checksum});
        }

        if (!applyResult.ok()) {
            exec(connection.get(), "ROLLBACK");
            return Result<Summary>::failure(
                ErrorCode::DependencyFailure,
                "migration " + migration.version + "_" + migration.name +
                    " failed and was rolled back: " + applyResult.error().detail);
        }

        if (auto committed = exec(connection.get(), "COMMIT"); !committed.ok()) {
            exec(connection.get(), "ROLLBACK");
            return Result<Summary>::failure(committed.error());
        }

        ++summary.applied;
    }

    spdlog::info("migrations complete: {} applied, {} already present",
                 summary.applied,
                 summary.alreadyApplied);
    return Result<Summary>::success(summary);
}

} // namespace launcher::migrations
