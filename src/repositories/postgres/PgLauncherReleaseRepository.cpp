#include "repositories/postgres/PgLauncherReleaseRepository.h"

#include <utility>

#include "repositories/postgres/PgSupport.h"

namespace launcher::repositories::postgres {
namespace {

domain::LauncherRelease mapRelease(const drogon::orm::Row& row, const domain::ReleaseQuery& query) {
    domain::LauncherRelease release;
    release.id = row["id"].as<std::string>();
    release.document = row["document"].as<std::string>();
    release.signature = row["signature"].as<std::string>();
    release.storageKey = row["storage_key"].as<std::string>();
    release.createdAt = row["created_at"].as<std::string>();
    release.retiredAt =
        row["retired_at"].isNull() ? std::string{} : row["retired_at"].as<std::string>();

    // The parsed view is rebuilt from the columns rather than by parsing `document` again. The
    // columns were extracted from those exact bytes when the release was published and nothing
    // writes them separately, so this is a cheaper reading of the same fact — and the bytes
    // themselves still travel verbatim, which is the only copy a client ever verifies.
    auto& parsed = release.parsed;
    parsed.schema = domain::RELEASE_DOCUMENT_SCHEMA;
    parsed.channel = query.channel;
    parsed.platform = query.platform;
    parsed.arch = query.arch;
    parsed.version.text = row["version"].as<std::string>();
    parsed.version.major = row["version_major"].as<int>();
    parsed.version.minor = row["version_minor"].as<int>();
    parsed.version.patch = row["version_patch"].as<int>();
    parsed.artifactSha256 = row["artifact_sha256"].as<std::string>();
    parsed.artifactSize = row["artifact_size"].as<int64_t>();
    parsed.releasedAt = row["released_at"].as<std::string>();
    return release;
}

} // namespace

PgLauncherReleaseRepository::PgLauncherReleaseRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<std::optional<domain::LauncherRelease>>
PgLauncherReleaseRepository::findLatest(domain::ReleaseQuery query) const {
    const auto rows = co_await database_->execSqlCoro(
        R"(
            SELECT id::text AS id, version, version_major, version_minor, version_patch,
                   artifact_sha256, artifact_size, storage_key, document, signature,
                   to_char(released_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"')
                       AS released_at,
                   to_char(created_at  AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"')
                       AS created_at,
                   to_char(retired_at  AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"')
                       AS retired_at
            FROM launcher_releases
            WHERE channel = $1 AND platform = $2 AND arch = $3 AND retired_at IS NULL
            ORDER BY version_major DESC, version_minor DESC, version_patch DESC
            LIMIT 1
        )",
        domain::nameFor(query.channel),
        domain::nameFor(query.platform),
        domain::nameFor(query.arch));

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapRelease(rows[0], query);
}

} // namespace launcher::repositories::postgres
