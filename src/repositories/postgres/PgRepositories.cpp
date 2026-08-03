#include "repositories/postgres/PgRepositories.h"

#include <utility>

namespace launcher::repositories {

const char* toDatabaseValue(UserTokenPurpose purpose) {
    switch (purpose) {
    case UserTokenPurpose::PasswordReset:
        return "password_reset";
    case UserTokenPurpose::EmailVerification:
        break;
    }
    return "email_verification";
}

} // namespace launcher::repositories

namespace launcher::repositories::postgres {
namespace {

using common::ErrorCode;
using common::Result;

constexpr const char* USER_COLUMNS =
    "id, email, display_name, password_hash, "
    "(email_verified_at IS NOT NULL) AS email_verified, is_active, "
    "upload_quota_bytes, upload_used_bytes";

domain::User mapUser(const drogon::orm::Row& row) {
    domain::User user;
    user.id = row["id"].as<std::string>();
    user.email = row["email"].as<std::string>();
    user.displayName = row["display_name"].as<std::string>();
    user.passwordHash = row["password_hash"].as<std::string>();
    user.emailVerified = row["email_verified"].as<bool>();
    user.isActive = row["is_active"].as<bool>();
    user.uploadQuotaBytes = row["upload_quota_bytes"].as<int64_t>();
    user.uploadUsedBytes = row["upload_used_bytes"].as<int64_t>();
    return user;
}

std::vector<std::string> firstColumn(const drogon::orm::Result& rows) {
    std::vector<std::string> values;
    values.reserve(rows.size());
    for (const auto& row : rows) {
        values.push_back(row[0].as<std::string>());
    }
    return values;
}

} // namespace

// ---------------------------------------------------------------------------
// Users
// ---------------------------------------------------------------------------

PgUserRepository::PgUserRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<std::optional<domain::User>> PgUserRepository::findById(std::string id) const {
    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + USER_COLUMNS + " FROM users WHERE id = $1::uuid", id);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapUser(rows[0]);
}

drogon::Task<std::optional<domain::User>> PgUserRepository::findByEmail(std::string email) const {
    // email is citext, so the comparison is already case-insensitive in the database.
    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + USER_COLUMNS + " FROM users WHERE email = $1", email);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapUser(rows[0]);
}

drogon::Task<Result<domain::User>> PgUserRepository::create(domain::NewUser user) const {
    // ON CONFLICT rather than catching a unique violation: the outcome is decided by the
    // index either way, but this reports it as an ordinary empty result instead of relying
    // on the exact exception type the driver happens to throw for SQLSTATE 23505.
    const auto rows = co_await database_->execSqlCoro(
        std::string("INSERT INTO users (email, display_name, password_hash) "
                    "VALUES ($1, $2, $3) ON CONFLICT (email) DO NOTHING RETURNING ") +
            USER_COLUMNS,
        user.email,
        user.displayName,
        user.passwordHash);

    if (rows.empty()) {
        co_return Result<domain::User>::failure(ErrorCode::Conflict,
                                                "that email address is already registered");
    }

    co_return Result<domain::User>::success(mapUser(rows[0]));
}

drogon::Task<void> PgUserRepository::markEmailVerified(std::string userId) const {
    co_await database_->execSqlCoro("UPDATE users SET email_verified_at = now() "
                                    "WHERE id = $1::uuid AND email_verified_at IS NULL",
                                    userId);
    co_return;
}

drogon::Task<void> PgUserRepository::updatePasswordHash(std::string userId,
                                                        std::string passwordHash) const {
    co_await database_->execSqlCoro(
        "UPDATE users SET password_hash = $2 WHERE id = $1::uuid", userId, passwordHash);
    co_return;
}

drogon::Task<void> PgUserRepository::recordSuccessfulLogin(std::string userId) const {
    co_await database_->execSqlCoro("UPDATE users SET last_login_at = now() WHERE id = $1::uuid",
                                    userId);
    co_return;
}

drogon::Task<bool> PgUserRepository::chargeUpload(std::string userId, int64_t bytes) const {
    // The quota test lives in the WHERE clause rather than in a preceding SELECT: two uploads
    // finishing at once would each see the same free space, and both would be allowed to use
    // it. Here at most one of them updates the row.
    const auto rows = co_await database_->execSqlCoro(
        "UPDATE users SET upload_used_bytes = upload_used_bytes + $2 "
        "WHERE id = $1::uuid AND upload_used_bytes + $2 <= upload_quota_bytes "
        "RETURNING upload_used_bytes",
        userId,
        std::to_string(bytes));

    co_return !rows.empty();
}

drogon::Task<void> PgUserRepository::releaseUpload(std::string userId, int64_t bytes) const {
    co_await database_->execSqlCoro(
        "UPDATE users SET upload_used_bytes = GREATEST(0, upload_used_bytes - $2) "
        "WHERE id = $1::uuid",
        userId,
        std::to_string(bytes));
    co_return;
}

// ---------------------------------------------------------------------------
// Roles and permissions
// ---------------------------------------------------------------------------

PgRoleRepository::PgRoleRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<std::vector<std::string>>
PgRoleRepository::permissionsForUser(std::string userId) const {
    const auto rows =
        co_await database_->execSqlCoro("SELECT DISTINCT p.key "
                                        "FROM user_roles ur "
                                        "JOIN role_permissions rp ON rp.role_id = ur.role_id "
                                        "JOIN permissions p ON p.id = rp.permission_id "
                                        "WHERE ur.user_id = $1::uuid "
                                        "ORDER BY p.key",
                                        userId);

    co_return firstColumn(rows);
}

drogon::Task<std::vector<std::string>> PgRoleRepository::rolesForUser(std::string userId) const {
    const auto rows = co_await database_->execSqlCoro(
        "SELECT r.key FROM user_roles ur JOIN roles r ON r.id = ur.role_id "
        "WHERE ur.user_id = $1::uuid ORDER BY r.key",
        userId);

    co_return firstColumn(rows);
}

drogon::Task<bool> PgRoleRepository::assignRole(std::string userId,
                                                std::string roleKey,
                                                std::optional<std::string> grantedBy) const {
    // ON CONFLICT DO NOTHING makes the grant idempotent; the SELECT source makes it a no-op
    // when the role key does not exist, which the follow-up query then reports.
    if (grantedBy.has_value()) {
        co_await database_->execSqlCoro(
            "INSERT INTO user_roles (user_id, role_id, granted_by) "
            "SELECT $1::uuid, r.id, $3::uuid FROM roles r WHERE r.key = $2 "
            "ON CONFLICT (user_id, role_id) DO NOTHING",
            userId,
            roleKey,
            *grantedBy);
    } else {
        co_await database_->execSqlCoro("INSERT INTO user_roles (user_id, role_id) "
                                        "SELECT $1::uuid, r.id FROM roles r WHERE r.key = $2 "
                                        "ON CONFLICT (user_id, role_id) DO NOTHING",
                                        userId,
                                        roleKey);
    }

    const auto rows = co_await database_->execSqlCoro(
        "SELECT 1 FROM user_roles ur JOIN roles r ON r.id = ur.role_id "
        "WHERE ur.user_id = $1::uuid AND r.key = $2",
        userId,
        roleKey);

    co_return !rows.empty();
}

// ---------------------------------------------------------------------------
// Refresh tokens
// ---------------------------------------------------------------------------

namespace {

constexpr const char* INSERT_REFRESH_TOKEN =
    "INSERT INTO refresh_tokens "
    "(user_id, family_id, token_hash, expires_at, user_agent, ip_address) "
    "VALUES ($1::uuid, $2::uuid, $3, now() + make_interval(secs => $4::double precision), "
    "        NULLIF($5, ''), NULLIF($6, '')::inet) "
    "RETURNING id";

} // namespace

PgRefreshTokenRepository::PgRefreshTokenRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<std::string> PgRefreshTokenRepository::issue(NewRefreshToken token) const {
    const auto rows = co_await database_->execSqlCoro(INSERT_REFRESH_TOKEN,
                                                      token.userId,
                                                      token.familyId,
                                                      token.tokenHash,
                                                      static_cast<double>(token.ttl.count()),
                                                      token.userAgent,
                                                      token.ipAddress);

    co_return rows[0]["id"].as<std::string>();
}

drogon::Task<std::optional<RefreshTokenRecord>>
PgRefreshTokenRepository::findByHash(std::string tokenHash) const {
    const auto rows = co_await database_->execSqlCoro("SELECT id, user_id, family_id, "
                                                      "       (revoked_at IS NOT NULL) AS revoked, "
                                                      "       (expires_at <= now()) AS expired "
                                                      "FROM refresh_tokens WHERE token_hash = $1",
                                                      tokenHash);

    if (rows.empty()) {
        co_return std::nullopt;
    }

    RefreshTokenRecord record;
    record.id = rows[0]["id"].as<std::string>();
    record.userId = rows[0]["user_id"].as<std::string>();
    record.familyId = rows[0]["family_id"].as<std::string>();
    record.revoked = rows[0]["revoked"].as<bool>();
    record.expired = rows[0]["expired"].as<bool>();
    co_return record;
}

namespace {

/// Insert-then-revoke as a single statement, using data-modifying CTEs.
///
/// A Drogon transaction is deliberately *not* used here: it commits asynchronously when the
/// transaction object is destroyed, so the new token could still be uncommitted at the
/// moment it is handed back to the client — which then presents a token the next request
/// cannot find. One statement is its own implicit transaction, atomic and already durable
/// when the query returns.
constexpr const char* ROTATE_REFRESH_TOKEN =
    "WITH inserted AS ("
    "  INSERT INTO refresh_tokens "
    "  (user_id, family_id, token_hash, expires_at, user_agent, ip_address) "
    "  VALUES ($1::uuid, $2::uuid, $3, now() + make_interval(secs => $4::double precision), "
    "          NULLIF($5, ''), NULLIF($6, '')::inet) "
    "  RETURNING id"
    "), revoked AS ("
    "  UPDATE refresh_tokens "
    "  SET revoked_at = now(), replaced_by = (SELECT id FROM inserted) "
    "  WHERE id = $7::uuid AND revoked_at IS NULL "
    "  RETURNING id"
    ") "
    "SELECT id FROM inserted";

} // namespace

drogon::Task<std::string> PgRefreshTokenRepository::rotate(std::string previousId,
                                                           NewRefreshToken next) const {
    const auto rows = co_await database_->execSqlCoro(ROTATE_REFRESH_TOKEN,
                                                      next.userId,
                                                      next.familyId,
                                                      next.tokenHash,
                                                      static_cast<double>(next.ttl.count()),
                                                      next.userAgent,
                                                      next.ipAddress,
                                                      previousId);

    co_return rows[0]["id"].as<std::string>();
}

drogon::Task<void> PgRefreshTokenRepository::revokeById(std::string id) const {
    co_await database_->execSqlCoro("UPDATE refresh_tokens SET revoked_at = now() "
                                    "WHERE id = $1::uuid AND revoked_at IS NULL",
                                    id);
    co_return;
}

drogon::Task<void> PgRefreshTokenRepository::revokeFamily(std::string familyId) const {
    co_await database_->execSqlCoro("UPDATE refresh_tokens SET revoked_at = now() "
                                    "WHERE family_id = $1::uuid AND revoked_at IS NULL",
                                    familyId);
    co_return;
}

drogon::Task<void> PgRefreshTokenRepository::revokeAllForUser(std::string userId) const {
    co_await database_->execSqlCoro("UPDATE refresh_tokens SET revoked_at = now() "
                                    "WHERE user_id = $1::uuid AND revoked_at IS NULL",
                                    userId);
    co_return;
}

drogon::Task<std::size_t> PgRefreshTokenRepository::deleteExpired() const {
    const auto rows =
        co_await database_->execSqlCoro("DELETE FROM refresh_tokens WHERE expires_at <= now()");
    co_return rows.affectedRows();
}

// ---------------------------------------------------------------------------
// Single-use user tokens
// ---------------------------------------------------------------------------

PgUserTokenRepository::PgUserTokenRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<void> PgUserTokenRepository::issue(std::string userId,
                                                UserTokenPurpose purpose,
                                                std::string tokenHash,
                                                std::chrono::seconds ttl) const {
    co_await database_->execSqlCoro(
        "INSERT INTO user_tokens (user_id, purpose, token_hash, expires_at) "
        "VALUES ($1::uuid, $2::user_token_purpose, $3, "
        "        now() + make_interval(secs => $4::double precision))",
        userId,
        std::string(toDatabaseValue(purpose)),
        tokenHash,
        static_cast<double>(ttl.count()));
    co_return;
}

drogon::Task<std::optional<std::string>>
PgUserTokenRepository::consume(std::string tokenHash, UserTokenPurpose purpose) const {
    // Single statement, so two concurrent redemptions of the same link cannot both win.
    const auto rows = co_await database_->execSqlCoro(
        "UPDATE user_tokens SET consumed_at = now() "
        "WHERE token_hash = $1 AND purpose = $2::user_token_purpose "
        "  AND consumed_at IS NULL AND expires_at > now() "
        "RETURNING user_id",
        tokenHash,
        std::string(toDatabaseValue(purpose)));

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return rows[0]["user_id"].as<std::string>();
}

drogon::Task<void> PgUserTokenRepository::invalidateAll(std::string userId,
                                                        UserTokenPurpose purpose) const {
    co_await database_->execSqlCoro(
        "UPDATE user_tokens SET consumed_at = now() "
        "WHERE user_id = $1::uuid AND purpose = $2::user_token_purpose AND consumed_at IS NULL",
        userId,
        std::string(toDatabaseValue(purpose)));
    co_return;
}

} // namespace launcher::repositories::postgres
