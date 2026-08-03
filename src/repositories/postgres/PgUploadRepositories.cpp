#include "repositories/postgres/PgUploadRepositories.h"

#include <json/json.h>

#include <utility>

#include "repositories/postgres/PgSupport.h"

namespace launcher::repositories {

const char* toDatabaseValue(UploadSessionStatus status) {
    switch (status) {
    case UploadSessionStatus::Completed:
        return "completed";
    case UploadSessionStatus::Aborted:
        return "aborted";
    case UploadSessionStatus::Pending:
        break;
    }
    return "pending";
}

} // namespace launcher::repositories

namespace launcher::repositories::postgres {
namespace {

using common::ErrorCode;
using common::Result;

constexpr const char* UPLOAD_SESSION_COLUMNS = R"(
    id, build_id, user_id, blob_sha256, declared_size_bytes, received_bytes,
    status::text AS status, (expires_at <= now()) AS expired
)";

UploadSession mapUploadSession(const drogon::orm::Row& row) {
    UploadSession session;
    session.id = row["id"].as<std::string>();
    session.buildId = row["build_id"].as<std::string>();
    session.userId = row["user_id"].as<std::string>();
    session.blobSha256 = row["blob_sha256"].as<std::string>();
    session.declaredSizeBytes = row["declared_size_bytes"].as<int64_t>();
    session.receivedBytes = row["received_bytes"].as<int64_t>();

    const auto status = row["status"].as<std::string>();
    session.status = status == "completed" ? UploadSessionStatus::Completed
                     : status == "aborted" ? UploadSessionStatus::Aborted
                                           : UploadSessionStatus::Pending;
    session.expired = row["expired"].as<bool>();
    return session;
}

} // namespace

// ---------------------------------------------------------------------------
// Blobs
// ---------------------------------------------------------------------------

PgBlobRepository::PgBlobRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<std::vector<std::string>>
PgBlobRepository::findMissing(std::vector<std::string> sha256s) const {
    std::vector<std::string> missing;
    if (sha256s.empty()) {
        co_return missing;
    }

    const auto rows = co_await database_->execSqlCoro(
        R"(SELECT t.value AS sha256
           FROM jsonb_array_elements_text($1::jsonb) WITH ORDINALITY AS t(value, position)
           WHERE NOT EXISTS (SELECT 1 FROM blobs b WHERE b.sha256 = t.value)
           ORDER BY t.position)",
        jsonArrayParameter(sha256s));

    missing.reserve(rows.size());
    for (const auto& row : rows) {
        missing.push_back(row["sha256"].as<std::string>());
    }
    co_return missing;
}

drogon::Task<std::optional<BlobRecord>> PgBlobRepository::findBySha256(std::string sha256) const {
    const auto rows = co_await database_->execSqlCoro(
        "SELECT sha256, size_bytes, storage_key FROM blobs WHERE sha256 = $1", sha256);

    if (rows.empty()) {
        co_return std::nullopt;
    }

    BlobRecord record;
    record.sha256 = rows[0]["sha256"].as<std::string>();
    record.sizeBytes = rows[0]["size_bytes"].as<int64_t>();
    record.storageKey = rows[0]["storage_key"].as<std::string>();
    co_return record;
}

drogon::Task<std::vector<BlobRecord>>
PgBlobRepository::findMany(std::vector<std::string> sha256s) const {
    std::vector<BlobRecord> records;
    if (sha256s.empty()) {
        co_return records;
    }

    const auto rows = co_await database_->execSqlCoro(
        R"(SELECT b.sha256, b.size_bytes, b.storage_key
           FROM blobs b
           WHERE b.sha256 IN (SELECT t.value FROM jsonb_array_elements_text($1::jsonb) AS t(value)))",
        jsonArrayParameter(sha256s));

    records.reserve(rows.size());
    for (const auto& row : rows) {
        BlobRecord record;
        record.sha256 = row["sha256"].as<std::string>();
        record.sizeBytes = row["size_bytes"].as<int64_t>();
        record.storageKey = row["storage_key"].as<std::string>();
        records.push_back(std::move(record));
    }
    co_return records;
}

drogon::Task<bool> PgBlobRepository::record(std::string sha256,
                                            int64_t sizeBytes,
                                            std::string storageKey,
                                            std::optional<std::string> uploadedByUserId) const {
    const auto rows = co_await database_->execSqlCoro(
        "INSERT INTO blobs (sha256, size_bytes, storage_key, uploaded_by_user_id) "
        "VALUES ($1, $2, $3, $4::uuid) ON CONFLICT (sha256) DO NOTHING RETURNING sha256",
        sha256,
        number(sizeBytes),
        storageKey,
        uploadedByUserId);

    co_return !rows.empty();
}

// ---------------------------------------------------------------------------
// Upload sessions
// ---------------------------------------------------------------------------

PgUploadSessionRepository::PgUploadSessionRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<Result<UploadSession>>
PgUploadSessionRepository::create(NewUploadSession session) const {
    // The partial unique index on open sessions is what makes a retried negotiation resume
    // the upload already in flight instead of opening a second staging file for it.
    const auto inserted = co_await database_->execSqlCoro(std::string(R"(
            INSERT INTO upload_sessions
                (build_id, user_id, blob_sha256, declared_size_bytes, expires_at)
            VALUES ($1::uuid, $2::uuid, $3, $4,
                    now() + make_interval(secs => $5::double precision))
            ON CONFLICT (build_id, blob_sha256) WHERE status = 'pending' DO NOTHING
            RETURNING )") + UPLOAD_SESSION_COLUMNS,
                                                          session.buildId,
                                                          session.userId,
                                                          session.blobSha256,
                                                          number(session.declaredSizeBytes),
                                                          static_cast<double>(session.ttl.count()));

    if (!inserted.empty()) {
        co_return Result<UploadSession>::success(mapUploadSession(inserted[0]));
    }

    const auto existing = co_await database_->execSqlCoro(
        std::string("SELECT ") + UPLOAD_SESSION_COLUMNS +
            " FROM upload_sessions"
            " WHERE build_id = $1::uuid AND blob_sha256 = $2 AND status = 'pending'",
        session.buildId,
        session.blobSha256);

    if (existing.empty()) {
        co_return Result<UploadSession>::failure(
            ErrorCode::Conflict, "this blob has just finished uploading for that build");
    }
    co_return Result<UploadSession>::success(mapUploadSession(existing[0]));
}

drogon::Task<std::optional<UploadSession>>
PgUploadSessionRepository::findById(std::string id) const {
    const auto rows =
        co_await database_->execSqlCoro(std::string("SELECT ") + UPLOAD_SESSION_COLUMNS +
                                            " FROM upload_sessions WHERE id = $1::uuid",
                                        id);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapUploadSession(rows[0]);
}

drogon::Task<std::optional<UploadSession>> PgUploadSessionRepository::reserveBytes(
    std::string id, int64_t expectedOffset, int64_t byteCount) const {
    // The offset is handed out by this statement, not read from the file on disk: two chunks
    // racing at the same offset cannot both match `received_bytes = $2`, so only one of them
    // is ever told to write.
    const auto rows = co_await database_->execSqlCoro(std::string(R"(
            UPDATE upload_sessions SET received_bytes = received_bytes + $3
            WHERE id = $1::uuid
              AND status = 'pending'
              AND expires_at > now()
              AND received_bytes = $2
              AND received_bytes + $3 <= declared_size_bytes
            RETURNING )") + UPLOAD_SESSION_COLUMNS,
                                                      id,
                                                      number(expectedOffset),
                                                      number(byteCount));

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapUploadSession(rows[0]);
}

drogon::Task<bool> PgUploadSessionRepository::markCompleted(std::string id) const {
    const auto rows =
        co_await database_->execSqlCoro("UPDATE upload_sessions SET status = 'completed' "
                                        "WHERE id = $1::uuid AND status = 'pending' RETURNING id",
                                        id);

    co_return !rows.empty();
}

drogon::Task<bool> PgUploadSessionRepository::markAborted(std::string id) const {
    const auto rows =
        co_await database_->execSqlCoro("UPDATE upload_sessions SET status = 'aborted' "
                                        "WHERE id = $1::uuid AND status = 'pending' RETURNING id",
                                        id);

    co_return !rows.empty();
}

drogon::Task<int64_t> PgUploadSessionRepository::countOpenForUser(std::string userId) const {
    const auto rows = co_await database_->execSqlCoro(
        "SELECT count(*) AS open_sessions FROM upload_sessions "
        "WHERE user_id = $1::uuid AND status = 'pending' AND expires_at > now()",
        userId);

    co_return rows.empty() ? 0 : rows[0]["open_sessions"].as<int64_t>();
}

drogon::Task<std::vector<UploadSession>> PgUploadSessionRepository::findExpired(int limit) const {
    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + UPLOAD_SESSION_COLUMNS +
            " FROM upload_sessions WHERE status = 'pending' AND expires_at <= now()"
            " ORDER BY expires_at LIMIT $1",
        number(limit));

    std::vector<UploadSession> sessions;
    sessions.reserve(rows.size());
    for (const auto& row : rows) {
        sessions.push_back(mapUploadSession(row));
    }
    co_return sessions;
}

drogon::Task<bool> PgUploadSessionRepository::deleteById(std::string id) const {
    const auto rows =
        co_await database_->execSqlCoro("DELETE FROM upload_sessions WHERE id = $1::uuid "
                                        "RETURNING id",
                                        id);

    co_return !rows.empty();
}

} // namespace launcher::repositories::postgres
