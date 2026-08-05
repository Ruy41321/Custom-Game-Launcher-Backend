#include "repositories/postgres/PgCrashReportRepository.h"

#include <utility>

#include "repositories/postgres/PgSupport.h"

namespace launcher::repositories::postgres {
namespace {

constexpr const char* CRASH_COLUMNS = R"(
    id, kind,
    to_char(occurred_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS occurred_at,
    launcher_version, platform, exception_type, message, stack_trace, fingerprint,
    to_char(received_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS received_at
)";

domain::CrashReport mapReport(const drogon::orm::Row& row) {
    domain::CrashReport report;
    report.id = row["id"].as<std::string>();
    report.kind = row["kind"].as<std::string>();
    report.occurredAt = row["occurred_at"].as<std::string>();
    report.launcherVersion = row["launcher_version"].as<std::string>();
    report.platform = row["platform"].as<std::string>();
    report.exceptionType = row["exception_type"].as<std::string>();
    report.message = row["message"].as<std::string>();
    report.stackTrace = row["stack_trace"].as<std::string>();
    report.fingerprint = row["fingerprint"].as<std::string>();
    report.receivedAt = row["received_at"].as<std::string>();
    return report;
}

} // namespace

PgCrashReportRepository::PgCrashReportRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<domain::CrashReport> PgCrashReportRepository::add(domain::NewCrashReport report,
                                                               std::string fingerprint) const {
    const auto rows = co_await database_->execSqlCoro(std::string(R"(
            INSERT INTO crash_reports
                (kind, occurred_at, launcher_version, platform, exception_type, message,
                 stack_trace, fingerprint)
            VALUES ($1, $2::timestamptz, $3, $4, $5, $6, $7, $8)
            RETURNING )") + CRASH_COLUMNS,
                                                      report.kind,
                                                      report.occurredAt,
                                                      report.launcherVersion,
                                                      report.platform,
                                                      report.exceptionType,
                                                      report.message,
                                                      report.stackTrace,
                                                      fingerprint);

    co_return mapReport(rows[0]);
}

drogon::Task<CrashPage> PgCrashReportRepository::search(CrashQuery query) const {
    const std::string filter = " WHERE ($1 = '' OR fingerprint = $1) ";

    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + CRASH_COLUMNS + " FROM crash_reports " + filter +
            " ORDER BY received_at DESC, id LIMIT $2 OFFSET $3",
        query.fingerprint,
        number(query.limit),
        number(query.offset));

    CrashPage page;
    page.items.reserve(rows.size());
    for (const auto& row : rows) {
        page.items.push_back(mapReport(row));
    }

    const auto totals = co_await database_->execSqlCoro(
        std::string("SELECT count(*) AS total FROM crash_reports ") + filter, query.fingerprint);
    page.total = totals[0]["total"].as<int64_t>();
    co_return page;
}

drogon::Task<CrashGroupPage> PgCrashReportRepository::groups(int limit, int offset) const {
    // The exception type and message of the *most recent* report in each group, rather than an
    // arbitrary one: a bug's newest occurrence is the one an operator is about to look at, and
    // `DISTINCT ON` is what gets it without a second query per row.
    const auto rows = co_await database_->execSqlCoro(
        R"(
            WITH latest AS (
                SELECT DISTINCT ON (fingerprint)
                       fingerprint, id, exception_type, message, received_at
                FROM crash_reports
                ORDER BY fingerprint, received_at DESC, id
            ),
            counted AS (
                SELECT fingerprint,
                       count(*)         AS occurrences,
                       min(received_at) AS first_seen,
                       max(received_at) AS last_seen
                FROM crash_reports
                GROUP BY fingerprint
            )
            SELECT l.fingerprint, l.id::text AS latest_report_id,
                   l.exception_type, l.message, c.occurrences,
                   to_char(c.first_seen AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"')
                       AS first_seen_at,
                   to_char(c.last_seen AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"')
                       AS last_seen_at
            FROM latest l
            JOIN counted c ON c.fingerprint = l.fingerprint
            ORDER BY c.last_seen DESC, l.fingerprint
            LIMIT $1 OFFSET $2
        )",
        number(limit),
        number(offset));

    CrashGroupPage page;
    page.items.reserve(rows.size());
    for (const auto& row : rows) {
        domain::CrashGroup group;
        group.fingerprint = row["fingerprint"].as<std::string>();
        group.latestReportId = row["latest_report_id"].as<std::string>();
        group.exceptionType = row["exception_type"].as<std::string>();
        group.message = row["message"].as<std::string>();
        group.occurrences = row["occurrences"].as<int64_t>();
        group.firstSeenAt = row["first_seen_at"].as<std::string>();
        group.lastSeenAt = row["last_seen_at"].as<std::string>();
        page.items.push_back(std::move(group));
    }

    const auto totals = co_await database_->execSqlCoro(
        "SELECT count(DISTINCT fingerprint) AS total FROM crash_reports");
    page.total = totals[0]["total"].as<int64_t>();
    co_return page;
}

drogon::Task<std::optional<domain::CrashReport>>
PgCrashReportRepository::findById(std::string id) const {
    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + CRASH_COLUMNS + " FROM crash_reports WHERE id = $1::uuid", id);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapReport(rows[0]);
}

drogon::Task<std::size_t> PgCrashReportRepository::deleteOlderThan(uint32_t seconds) const {
    // The window is one text parameter turned into an interval rather than a value
    // interpolated into the statement, as the analytics report does for the same reason.
    const auto rows = co_await database_->execSqlCoro(
        "DELETE FROM crash_reports WHERE received_at < now() - $1::interval RETURNING id",
        number(seconds) + " seconds");

    co_return rows.size();
}

} // namespace launcher::repositories::postgres
