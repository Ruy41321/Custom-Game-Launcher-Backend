#include "repositories/postgres/PgAdminRepositories.h"

#include <json/json.h>

#include <sstream>
#include <utility>

#include "repositories/postgres/PgSupport.h"

namespace launcher::repositories::postgres {
namespace {

/// Deliberately without `password_hash`. The domain user carries one because authentication
/// needs it; the operator console never does, and a column that is never selected cannot be
/// serialised into an administrative response by accident.
constexpr const char* ADMIN_USER_COLUMNS = R"(
    u.id, u.email, u.display_name,
    (u.email_verified_at IS NOT NULL) AS email_verified, u.is_active,
    u.password_change_required, u.upload_quota_bytes, u.upload_used_bytes,
    to_char(u.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
    COALESCE(to_char(u.last_login_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'), '')
        AS last_login_at,
    COALESCE((SELECT string_agg(r.key, ',' ORDER BY r.key)
              FROM user_roles ur JOIN roles r ON r.id = ur.role_id
              WHERE ur.user_id = u.id), '') AS role_keys
)";

/// The audit arm every mutation carries. It selects from the arm that changed something, so a
/// statement whose change did not apply writes no row.
constexpr const char* AUDIT_INSERT =
    "INSERT INTO audit_log (actor_user_id, action, entity_type, entity_id, metadata) ";

std::vector<std::string> splitRoles(const std::string& packed) {
    std::vector<std::string> roles;
    std::istringstream stream(packed);
    std::string role;
    while (std::getline(stream, role, ',')) {
        if (!role.empty()) {
            roles.push_back(role);
        }
    }
    return roles;
}

AdminUserSummary mapSummary(const drogon::orm::Row& row) {
    AdminUserSummary summary;
    summary.user.id = row["id"].as<std::string>();
    summary.user.email = row["email"].as<std::string>();
    summary.user.displayName = row["display_name"].as<std::string>();
    summary.user.emailVerified = row["email_verified"].as<bool>();
    summary.user.isActive = row["is_active"].as<bool>();
    summary.user.passwordChangeRequired = row["password_change_required"].as<bool>();
    summary.user.uploadQuotaBytes = row["upload_quota_bytes"].as<int64_t>();
    summary.user.uploadUsedBytes = row["upload_used_bytes"].as<int64_t>();
    summary.createdAt = row["created_at"].as<std::string>();
    summary.lastLoginAt = row["last_login_at"].as<std::string>();
    summary.roles = splitRoles(row["role_keys"].as<std::string>());
    return summary;
}

domain::AuditEntry mapAuditEntry(const drogon::orm::Row& row) {
    domain::AuditEntry entry;
    entry.id = row["id"].as<int64_t>();
    entry.actorUserId = row["actor_user_id"].as<std::string>();
    entry.actorEmail = row["actor_email"].as<std::string>();
    entry.action = row["action"].as<std::string>();
    entry.entityType = row["entity_type"].as<std::string>();
    entry.entityId = row["entity_id"].as<std::string>();
    entry.metadataJson = row["metadata"].as<std::string>();
    entry.createdAt = row["created_at"].as<std::string>();
    return entry;
}

RoleChange classifyRoleChange(const drogon::orm::Row& row) {
    if (row["user_exists"].as<int64_t>() == 0) {
        return RoleChange::NoSuchUser;
    }
    if (row["role_exists"].as<int64_t>() == 0) {
        return RoleChange::NoSuchRole;
    }
    return row["changed"].as<int64_t>() > 0 ? RoleChange::Applied : RoleChange::AlreadyInThatState;
}

} // namespace

// ---------------------------------------------------------------------------
// Users
// ---------------------------------------------------------------------------

PgAdminUserRepository::PgAdminUserRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<AdminUserPage> PgAdminUserRepository::search(AdminUserQuery query) const {
    // A blank search term is an absent filter rather than a filter for a blank value, which is
    // why the condition tests the parameter itself before it tests the column.
    const std::string filter = " WHERE ($1 = '' OR u.email ILIKE '%' || $1 || '%'"
                               "        OR u.display_name ILIKE '%' || $1 || '%') "
                               "   AND (NOT $2::boolean OR NOT u.is_active) ";

    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + ADMIN_USER_COLUMNS + " FROM users u " + filter +
            " ORDER BY u.created_at DESC, u.id LIMIT $3 OFFSET $4",
        query.search,
        query.onlyInactive ? "true" : "false",
        number(query.limit),
        number(query.offset));

    AdminUserPage page;
    page.items.reserve(rows.size());
    for (const auto& row : rows) {
        page.items.push_back(mapSummary(row));
    }

    const auto totals = co_await database_->execSqlCoro(
        std::string("SELECT count(*) AS total FROM users u ") + filter,
        query.search,
        query.onlyInactive ? "true" : "false");
    page.total = totals[0]["total"].as<int64_t>();
    co_return page;
}

drogon::Task<std::optional<AdminUserSummary>>
PgAdminUserRepository::findById(std::string userId) const {
    const auto rows = co_await database_->execSqlCoro(std::string("SELECT ") + ADMIN_USER_COLUMNS +
                                                          " FROM users u WHERE u.id = $1::uuid",
                                                      userId);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapSummary(rows[0]);
}

drogon::Task<std::optional<AdminUserSummary>> PgAdminUserRepository::setUploadQuota(
    std::string userId, int64_t quotaBytes, domain::NewAuditEntry audit) const {
    // The final SELECT reads the UPDATE's own RETURNING rather than the users table: a
    // data-modifying CTE's effects are invisible to the rest of the statement, so selecting
    // from `users` again would hand back the row as it was before the change.
    const auto rows = co_await database_->execSqlCoro(std::string(R"(
            WITH updated AS (
                UPDATE users SET upload_quota_bytes = $2 WHERE id = $1::uuid RETURNING *
            ),
            logged AS ()") + AUDIT_INSERT +
                                                          R"(
                SELECT NULLIF($3, '')::uuid, $4, $5, $6, $7::jsonb FROM updated
            )
            SELECT )" + ADMIN_USER_COLUMNS + " FROM updated u",
                                                      userId,
                                                      number(quotaBytes),
                                                      audit.actorUserId,
                                                      audit.action,
                                                      audit.entityType,
                                                      audit.entityId,
                                                      auditMetadataJson(audit.metadata));

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapSummary(rows[0]);
}

drogon::Task<std::optional<AdminUserSummary>> PgAdminUserRepository::setActive(
    std::string userId, bool active, domain::NewAuditEntry audit) const {
    // Deactivating revokes the account's live refresh tokens in the same statement. Leaving
    // them alive would mean a disabled account keeps renewing access tokens for as long as
    // somebody holds one, which is not what disabling an account means to anybody.
    const auto rows = co_await database_->execSqlCoro(std::string(R"(
            WITH updated AS (
                UPDATE users SET is_active = $2::boolean WHERE id = $1::uuid RETURNING *
            ),
            revoked AS (
                UPDATE refresh_tokens SET revoked_at = now()
                WHERE user_id = $1::uuid AND revoked_at IS NULL
                  AND NOT $2::boolean AND EXISTS (SELECT 1 FROM updated)
            ),
            logged AS ()") + AUDIT_INSERT +
                                                          R"(
                SELECT NULLIF($3, '')::uuid, $4, $5, $6, $7::jsonb FROM updated
            )
            SELECT )" + ADMIN_USER_COLUMNS + " FROM updated u",
                                                      userId,
                                                      active ? "true" : "false",
                                                      audit.actorUserId,
                                                      audit.action,
                                                      audit.entityType,
                                                      audit.entityId,
                                                      auditMetadataJson(audit.metadata));

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapSummary(rows[0]);
}

drogon::Task<std::optional<AdminUserSummary>> PgAdminUserRepository::setTemporaryPassword(
    std::string userId, std::string passwordHash, domain::NewAuditEntry audit) const {
    // Four effects, one statement, for D36's reason and one more of its own: a hash written
    // without the flag is a password nobody told the owner about that behaves like theirs, and
    // a flag written without the hash locks an account out with a password that still works.
    // The sessions and the outstanding reset links go with them — everything reachable with
    // the credential being replaced stops being reachable at the moment it is replaced.
    const auto rows = co_await database_->execSqlCoro(std::string(R"(
            WITH updated AS (
                UPDATE users
                SET password_hash = $2, password_change_required = true
                WHERE id = $1::uuid
                RETURNING *
            ),
            revoked AS (
                UPDATE refresh_tokens SET revoked_at = now()
                WHERE user_id = $1::uuid AND revoked_at IS NULL
                  AND EXISTS (SELECT 1 FROM updated)
            ),
            invalidated AS (
                UPDATE user_tokens SET consumed_at = now()
                WHERE user_id = $1::uuid AND purpose = 'password_reset' AND consumed_at IS NULL
                  AND EXISTS (SELECT 1 FROM updated)
            ),
            logged AS ()") + AUDIT_INSERT +
                                                          R"(
                SELECT NULLIF($3, '')::uuid, $4, $5, $6, $7::jsonb FROM updated
            )
            SELECT )" + ADMIN_USER_COLUMNS + " FROM updated u",
                                                      userId,
                                                      passwordHash,
                                                      audit.actorUserId,
                                                      audit.action,
                                                      audit.entityType,
                                                      audit.entityId,
                                                      auditMetadataJson(audit.metadata));

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapSummary(rows[0]);
}

drogon::Task<RoleChange> PgAdminUserRepository::grantRole(std::string userId,
                                                          std::string roleKey,
                                                          domain::NewAuditEntry audit) const {
    // `granted_by` and the audit actor are the same person by construction, which is the point
    // of writing both here rather than trusting two call sites to agree.
    const auto rows = co_await database_->execSqlCoro(std::string(R"(
            WITH target AS (SELECT id FROM users WHERE id = $1::uuid),
                 wanted AS (SELECT id FROM roles WHERE key = $2),
                 granted AS (
                     INSERT INTO user_roles (user_id, role_id, granted_by)
                     SELECT t.id, w.id, NULLIF($3, '')::uuid FROM target t, wanted w
                     ON CONFLICT DO NOTHING
                     RETURNING role_id
                 ),
                 logged AS ()") + AUDIT_INSERT +
                                                          R"(
                     SELECT NULLIF($3, '')::uuid, $4, $5, $6, $7::jsonb FROM granted
                 )
            SELECT (SELECT count(*) FROM target)  AS user_exists,
                   (SELECT count(*) FROM wanted)  AS role_exists,
                   (SELECT count(*) FROM granted) AS changed
        )",
                                                      userId,
                                                      roleKey,
                                                      audit.actorUserId,
                                                      audit.action,
                                                      audit.entityType,
                                                      audit.entityId,
                                                      auditMetadataJson(audit.metadata));

    co_return classifyRoleChange(rows[0]);
}

drogon::Task<RoleChange> PgAdminUserRepository::revokeRole(std::string userId,
                                                           std::string roleKey,
                                                           domain::NewAuditEntry audit) const {
    const auto rows = co_await database_->execSqlCoro(std::string(R"(
            WITH target AS (SELECT id FROM users WHERE id = $1::uuid),
                 wanted AS (SELECT id FROM roles WHERE key = $2),
                 revoked AS (
                     DELETE FROM user_roles
                     WHERE user_id = (SELECT id FROM target)
                       AND role_id = (SELECT id FROM wanted)
                     RETURNING role_id
                 ),
                 logged AS ()") + AUDIT_INSERT +
                                                          R"(
                     SELECT NULLIF($3, '')::uuid, $4, $5, $6, $7::jsonb FROM revoked
                 )
            SELECT (SELECT count(*) FROM target)  AS user_exists,
                   (SELECT count(*) FROM wanted)  AS role_exists,
                   (SELECT count(*) FROM revoked) AS changed
        )",
                                                      userId,
                                                      roleKey,
                                                      audit.actorUserId,
                                                      audit.action,
                                                      audit.entityType,
                                                      audit.entityId,
                                                      auditMetadataJson(audit.metadata));

    co_return classifyRoleChange(rows[0]);
}

drogon::Task<std::vector<domain::Role>> PgAdminUserRepository::listRoles() const {
    const auto rows =
        co_await database_->execSqlCoro("SELECT id, key, description FROM roles ORDER BY key");

    std::vector<domain::Role> roles;
    roles.reserve(rows.size());
    for (const auto& row : rows) {
        domain::Role role;
        role.id = row["id"].as<int>();
        role.key = row["key"].as<std::string>();
        role.description = row["description"].as<std::string>();
        roles.push_back(std::move(role));
    }
    co_return roles;
}

drogon::Task<int64_t>
PgAdminUserRepository::countOtherHoldersOf(std::string permissionKey,
                                           std::string excludingUserId) const {
    // Deactivated accounts do not count: an operator who cannot sign in is not a way back in.
    const auto rows = co_await database_->execSqlCoro(
        R"(
            SELECT count(DISTINCT u.id) AS holders
            FROM users u
            JOIN user_roles ur       ON ur.user_id = u.id
            JOIN role_permissions rp ON rp.role_id = ur.role_id
            JOIN permissions p       ON p.id = rp.permission_id
            WHERE p.key = $1 AND u.is_active AND u.id <> $2::uuid
        )",
        permissionKey,
        excludingUserId);

    co_return rows[0]["holders"].as<int64_t>();
}

// ---------------------------------------------------------------------------
// Download analytics
// ---------------------------------------------------------------------------

PgAnalyticsRepository::PgAnalyticsRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<DownloadReport> PgAnalyticsRepository::downloadReport(int days, int topGames) const {
    // The window is one text parameter turned into an interval, rather than a value
    // interpolated into the statement: the days come from a query string.
    const std::string window = number(days) + " days";

    const auto totals = co_await database_->execSqlCoro(
        R"(
            SELECT count(*)                                      AS downloads,
                   count(DISTINCT user_id)                       AS distinct_users,
                   COALESCE(sum(bytes_planned), 0)               AS bytes_planned,
                   count(*) FILTER (WHERE kind = 'full')         AS full_downloads,
                   count(*) FILTER (WHERE kind = 'delta')        AS delta_downloads
            FROM download_events
            WHERE created_at >= now() - $1::interval
        )",
        window);

    DownloadReport report;
    report.days = days;
    report.totals.downloads = totals[0]["downloads"].as<int64_t>();
    report.totals.distinctUsers = totals[0]["distinct_users"].as<int64_t>();
    report.totals.bytesPlanned = totals[0]["bytes_planned"].as<int64_t>();
    report.totals.fullDownloads = totals[0]["full_downloads"].as<int64_t>();
    report.totals.deltaDownloads = totals[0]["delta_downloads"].as<int64_t>();

    // generate_series supplies the days, so a day with no downloads is a zero rather than a
    // gap. A chart that skipped empty days would draw a busier picture than the truth.
    const auto daily = co_await database_->execSqlCoro(
        R"(
            SELECT to_char(d.day, 'YYYY-MM-DD')            AS day,
                   count(e.id)                             AS downloads,
                   COALESCE(sum(e.bytes_planned), 0)       AS bytes_planned
            FROM generate_series((now() AT TIME ZONE 'UTC')::date - $1::interval,
                                 (now() AT TIME ZONE 'UTC')::date,
                                 '1 day') AS d(day)
            LEFT JOIN download_events e
                   ON (e.created_at AT TIME ZONE 'UTC')::date = d.day::date
            GROUP BY d.day
            ORDER BY d.day
        )",
        window);

    report.daily.reserve(daily.size());
    for (const auto& row : daily) {
        DownloadDay entry;
        entry.day = row["day"].as<std::string>();
        entry.downloads = row["downloads"].as<int64_t>();
        entry.bytesPlanned = row["bytes_planned"].as<int64_t>();
        report.daily.push_back(std::move(entry));
    }

    const auto games = co_await database_->execSqlCoro(
        R"(
            SELECT g.id::text                              AS game_id,
                   g.slug, g.title,
                   count(*)                                AS downloads,
                   COALESCE(sum(e.bytes_planned), 0)       AS bytes_planned,
                   count(DISTINCT e.user_id)               AS distinct_users
            FROM download_events e
            JOIN games g ON g.id = e.game_id
            WHERE e.created_at >= now() - $1::interval
            GROUP BY g.id, g.slug, g.title
            ORDER BY downloads DESC, g.title
            LIMIT $2
        )",
        window,
        number(topGames));

    report.topGames.reserve(games.size());
    for (const auto& row : games) {
        GameDownloads entry;
        entry.gameId = row["game_id"].as<std::string>();
        entry.slug = row["slug"].as<std::string>();
        entry.title = row["title"].as<std::string>();
        entry.downloads = row["downloads"].as<int64_t>();
        entry.bytesPlanned = row["bytes_planned"].as<int64_t>();
        entry.distinctUsers = row["distinct_users"].as<int64_t>();
        report.topGames.push_back(std::move(entry));
    }

    co_return report;
}

// ---------------------------------------------------------------------------
// Audit trail
// ---------------------------------------------------------------------------

PgAuditRepository::PgAuditRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<AuditPage> PgAuditRepository::search(AuditQuery query) const {
    // Each filter is written as "the parameter is absent, or the column matches". The uuid one
    // goes through NULLIF twice rather than comparing the parameter to '' first: PostgreSQL is
    // free to evaluate `$1::uuid` even in a branch that cannot be reached, and casting an empty
    // string to uuid is an error, not a false.
    const std::string filter =
        " WHERE (NULLIF($1, '')::uuid IS NULL OR a.actor_user_id = NULLIF($1, '')::uuid) "
        "   AND ($2 = '' OR a.action = $2) "
        "   AND ($3 = '' OR a.entity_type = $3) "
        "   AND ($4 = '' OR a.entity_id = $4) ";

    const auto rows = co_await database_->execSqlCoro(std::string(R"(
            SELECT a.id,
                   COALESCE(a.actor_user_id::text, '') AS actor_user_id,
                   COALESCE(u.email, '')               AS actor_email,
                   a.action, a.entity_type,
                   COALESCE(a.entity_id, '')           AS entity_id,
                   a.metadata::text                    AS metadata,
                   to_char(a.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"')
                                                       AS created_at
            FROM audit_log a
            LEFT JOIN users u ON u.id = a.actor_user_id
        )") + filter + " ORDER BY a.created_at DESC, a.id DESC LIMIT $5 OFFSET $6",
                                                      query.actorUserId,
                                                      query.action,
                                                      query.entityType,
                                                      query.entityId,
                                                      number(query.limit),
                                                      number(query.offset));

    AuditPage page;
    page.items.reserve(rows.size());
    for (const auto& row : rows) {
        page.items.push_back(mapAuditEntry(row));
    }

    const auto totals = co_await database_->execSqlCoro(
        std::string("SELECT count(*) AS total FROM audit_log a ") + filter,
        query.actorUserId,
        query.action,
        query.entityType,
        query.entityId);
    page.total = totals[0]["total"].as<int64_t>();
    co_return page;
}

} // namespace launcher::repositories::postgres
