#include "repositories/postgres/PgDownloadRepositories.h"

#include <utility>

#include "repositories/postgres/PgSupport.h"

namespace launcher::repositories::postgres {

PgDownloadRepository::PgDownloadRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<bool> PgDownloadRepository::recordEvent(DownloadEvent event) const {
    // NULLIF rather than a second statement: a first install has no version to come from, and
    // the column is nullable precisely so that case is representable.
    const auto rows = co_await database_->execSqlCoro(
        R"(INSERT INTO download_events
               (game_id, build_id, user_id, from_version_id, kind, bytes_planned)
           VALUES ($1::uuid, $2::uuid, $3::uuid, NULLIF($4, '')::uuid, $5::download_kind, $6)
           RETURNING id)",
        event.gameId,
        event.buildId,
        event.userId,
        event.fromVersionId,
        std::string(domain::toString(event.kind)),
        number(event.bytesPlanned));

    co_return !rows.empty();
}

} // namespace launcher::repositories::postgres
