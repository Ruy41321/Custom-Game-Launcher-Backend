#include "repositories/postgres/PgCatalogRepositories.h"

#include <json/json.h>

#include <utility>

#include "repositories/postgres/PgSupport.h"

namespace launcher::repositories::postgres {
namespace {

using common::ErrorCode;
using common::Result;

constexpr const char* GAME_COLUMNS = R"(
    g.id, g.slug, g.title, g.summary, g.description, g.publisher_user_id,
    u.display_name AS publisher_display_name,
    COALESCE(to_char(g.release_date, 'YYYY-MM-DD'), '') AS release_date,
    g.visibility::text AS visibility,
    to_char(g.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
    to_char(g.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS updated_at,
    -- A correlated subquery rather than a join, so that every existing query picks the cover
    -- up by including these columns, including the two that select from a CTE. A game has at
    -- most one cover (game_media_single_kind_unique), so this cannot multiply rows either way.
    COALESCE((SELECT m.storage_key FROM game_media m
              WHERE m.game_id = g.id AND m.kind = 'cover'), '') AS cover_storage_key
)";

constexpr const char* VERSION_COLUMNS = R"(
    id, game_id, semver, version_major, version_minor, version_patch,
    stage::text AS stage, release_notes,
    COALESCE(to_char(published_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'), '')
        AS published_at,
    to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at
)";

constexpr const char* BUILD_COLUMNS = R"(
    id, game_version_id, platform::text AS platform, architecture::text AS architecture,
    status::text AS status, COALESCE(manifest_sha256, '') AS manifest_sha256,
    total_size_bytes, file_count,
    COALESCE(entrypoint_relative_path, '') AS entrypoint_relative_path, default_launch_args,
    to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
    COALESCE(to_char(ready_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'), '') AS ready_at
)";

constexpr const char* MEDIA_COLUMNS = R"(
    m.id, m.game_id, m.kind::text AS kind, m.storage_key, m.sha256, m.content_type,
    m.size_bytes, m.alt_text, m.sort_order,
    to_char(m.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at
)";

domain::GameMedia mapMedia(const drogon::orm::Row& row) {
    domain::GameMedia media;
    media.id = row["id"].as<std::string>();
    media.gameId = row["game_id"].as<std::string>();
    media.kind = domain::parseMediaKind(row["kind"].as<std::string>())
                     .value_or(domain::MediaKind::Screenshot);
    media.storageKey = row["storage_key"].as<std::string>();
    media.sha256 = row["sha256"].as<std::string>();
    media.contentType = row["content_type"].as<std::string>();
    media.sizeBytes = row["size_bytes"].as<int64_t>();
    media.altText = row["alt_text"].as<std::string>();
    media.sortOrder = row["sort_order"].as<int>();
    media.createdAt = row["created_at"].as<std::string>();
    return media;
}

constexpr const char* PATCH_NOTE_COLUMNS = R"(
    n.id, n.game_id, COALESCE(n.game_version_id::text, '') AS game_version_id,
    n.title, n.body_markdown,
    COALESCE(n.author_user_id::text, '') AS author_user_id,
    COALESCE(a.display_name, '') AS author_display_name,
    COALESCE(to_char(n.published_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'), '')
        AS published_at,
    to_char(n.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
    to_char(n.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS updated_at
)";

/// The author is a LEFT JOIN because author_user_id is ON DELETE SET NULL: a devlog outlives
/// the account that wrote it, and a note whose author has gone is still worth reading.
constexpr const char* PATCH_NOTE_SOURCE =
    " FROM patch_notes n LEFT JOIN users a ON a.id = n.author_user_id ";

domain::PatchNote mapPatchNote(const drogon::orm::Row& row) {
    domain::PatchNote note;
    note.id = row["id"].as<std::string>();
    note.gameId = row["game_id"].as<std::string>();
    note.gameVersionId = row["game_version_id"].as<std::string>();
    note.title = row["title"].as<std::string>();
    note.bodyMarkdown = row["body_markdown"].as<std::string>();
    note.authorUserId = row["author_user_id"].as<std::string>();
    note.authorDisplayName = row["author_display_name"].as<std::string>();
    note.publishedAt = row["published_at"].as<std::string>();
    note.createdAt = row["created_at"].as<std::string>();
    note.updatedAt = row["updated_at"].as<std::string>();
    return note;
}

domain::Game mapGame(const drogon::orm::Row& row) {
    domain::Game game;
    game.id = row["id"].as<std::string>();
    game.slug = row["slug"].as<std::string>();
    game.title = row["title"].as<std::string>();
    game.summary = row["summary"].as<std::string>();
    game.description = row["description"].as<std::string>();
    game.publisherUserId = row["publisher_user_id"].as<std::string>();
    game.publisherDisplayName = row["publisher_display_name"].as<std::string>();
    game.releaseDate = row["release_date"].as<std::string>();
    game.visibility = domain::parseGameVisibility(row["visibility"].as<std::string>())
                          .value_or(domain::GameVisibility::Draft);
    game.createdAt = row["created_at"].as<std::string>();
    game.updatedAt = row["updated_at"].as<std::string>();
    game.coverStorageKey = row["cover_storage_key"].as<std::string>();
    return game;
}

domain::GameVersion mapVersion(const drogon::orm::Row& row) {
    domain::GameVersion version;
    version.id = row["id"].as<std::string>();
    version.gameId = row["game_id"].as<std::string>();
    version.semver = row["semver"].as<std::string>();
    version.versionMajor = row["version_major"].as<int>();
    version.versionMinor = row["version_minor"].as<int>();
    version.versionPatch = row["version_patch"].as<int>();
    version.stage = domain::parseBuildStage(row["stage"].as<std::string>())
                        .value_or(domain::BuildStage::Release);
    version.releaseNotes = row["release_notes"].as<std::string>();
    version.publishedAt = row["published_at"].as<std::string>();
    version.createdAt = row["created_at"].as<std::string>();
    return version;
}

domain::Build mapBuild(const drogon::orm::Row& row) {
    domain::Build build;
    build.id = row["id"].as<std::string>();
    build.gameVersionId = row["game_version_id"].as<std::string>();
    build.platform = domain::parseBuildPlatform(row["platform"].as<std::string>())
                         .value_or(domain::BuildPlatform::Windows);
    build.architecture = domain::parseBuildArchitecture(row["architecture"].as<std::string>())
                             .value_or(domain::BuildArchitecture::X64);
    build.status = domain::parseBuildStatus(row["status"].as<std::string>())
                       .value_or(domain::BuildStatus::Uploading);
    build.manifestSha256 = row["manifest_sha256"].as<std::string>();
    build.totalSizeBytes = row["total_size_bytes"].as<int64_t>();
    build.fileCount = row["file_count"].as<int32_t>();
    build.entrypointRelativePath = row["entrypoint_relative_path"].as<std::string>();
    build.defaultLaunchArgs = row["default_launch_args"].as<std::string>();
    build.createdAt = row["created_at"].as<std::string>();
    build.readyAt = row["ready_at"].as<std::string>();
    return build;
}

const char* orderByFor(GameSort sort) {
    switch (sort) {
    case GameSort::Title:
        return " ORDER BY lower(g.title) ASC, g.created_at DESC, g.id ";
    case GameSort::RecentlyAdded:
        return " ORDER BY g.created_at DESC, g.id ";
    case GameSort::ReleaseDate:
        break;
    }
    return " ORDER BY g.release_date DESC NULLS LAST, g.created_at DESC, g.id ";
}

} // namespace

// ---------------------------------------------------------------------------
// Games
// ---------------------------------------------------------------------------

PgGameRepository::PgGameRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<std::optional<domain::Game>> PgGameRepository::findById(std::string id) const {
    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + GAME_COLUMNS +
            " FROM games g JOIN users u ON u.id = g.publisher_user_id WHERE g.id = $1::uuid",
        id);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapGame(rows[0]);
}

drogon::Task<std::optional<domain::Game>> PgGameRepository::findBySlug(std::string slug) const {
    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + GAME_COLUMNS +
            " FROM games g JOIN users u ON u.id = g.publisher_user_id WHERE g.slug = $1",
        slug);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapGame(rows[0]);
}

drogon::Task<Result<domain::Game>> PgGameRepository::create(domain::NewGame game) const {
    // The publisher's display name comes from a join, which INSERT ... RETURNING cannot do,
    // so the insert feeds a CTE that the outer query joins as usual.
    const auto rows = co_await database_->execSqlCoro(
        std::string(R"(
            WITH inserted AS (
                INSERT INTO games
                    (slug, title, summary, description, publisher_user_id, release_date, visibility)
                VALUES ($1, $2, $3, $4, $5::uuid, NULLIF($6, '')::date, $7::game_visibility)
                ON CONFLICT (slug) DO NOTHING
                RETURNING *
            )
            SELECT )") +
            GAME_COLUMNS + " FROM inserted g JOIN users u ON u.id = g.publisher_user_id",
        game.slug,
        game.title,
        game.summary,
        game.description,
        game.publisherUserId,
        game.releaseDate,
        std::string(domain::toString(game.visibility)));

    if (rows.empty()) {
        co_return Result<domain::Game>::failure(ErrorCode::Conflict,
                                                "a game with that slug already exists");
    }
    co_return Result<domain::Game>::success(mapGame(rows[0]));
}

drogon::Task<std::optional<domain::Game>>
PgGameRepository::update(std::string id, domain::GameUpdate changes) const {
    std::optional<std::string> visibility;
    if (changes.visibility.has_value()) {
        visibility = domain::toString(*changes.visibility);
    }

    const auto rows = co_await database_->execSqlCoro(
        std::string(R"(
            WITH updated AS (
                UPDATE games SET
                    title = COALESCE($2, title),
                    summary = COALESCE($3, summary),
                    description = COALESCE($4, description),
                    release_date = CASE WHEN $5::text IS NULL THEN release_date
                                        WHEN $5 = '' THEN NULL
                                        ELSE $5::date END,
                    visibility = COALESCE($6::game_visibility, visibility)
                WHERE id = $1::uuid
                RETURNING *
            )
            SELECT )") +
            GAME_COLUMNS + " FROM updated g JOIN users u ON u.id = g.publisher_user_id",
        id,
        changes.title,
        changes.summary,
        changes.description,
        changes.releaseDate,
        visibility);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapGame(rows[0]);
}

drogon::Task<GamePage> PgGameRepository::search(GameQuery query) const {
    // `includeUnpublished` widens the result to drafts; every caller that sets it has already
    // narrowed the query to one publisher, which the service layer enforces.
    constexpr const char* FILTER = R"(
        WHERE (g.visibility = 'public' OR $1::boolean)
          AND ($2::text IS NULL OR g.publisher_user_id = $2::uuid)
          AND ($3::text IS NULL OR g.title ILIKE '%' || $3 || '%')
    )";

    const std::optional<std::string> publisher =
        query.publisherUserId.empty() ? std::nullopt : std::optional{query.publisherUserId};
    const std::optional<std::string> search =
        query.search.empty() ? std::nullopt : std::optional{query.search};

    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + GAME_COLUMNS +
            " FROM games g JOIN users u ON u.id = g.publisher_user_id " + FILTER +
            orderByFor(query.sort) + " LIMIT $4 OFFSET $5",
        query.includeUnpublished,
        publisher,
        search,
        number(query.limit),
        number(query.offset));

    // A second statement rather than a window function: `count(*) OVER ()` reports zero for a
    // page past the end of the result, which would make the client's page count wrong exactly
    // when it needs it.
    const auto totals = co_await database_->execSqlCoro(
        std::string("SELECT count(*) AS total FROM games g ") + FILTER,
        query.includeUnpublished,
        publisher,
        search);

    GamePage page;
    page.total = totals.empty() ? 0 : totals[0]["total"].as<int64_t>();
    page.items.reserve(rows.size());
    for (const auto& row : rows) {
        page.items.push_back(mapGame(row));
    }
    co_return page;
}

// ---------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------

PgGameVersionRepository::PgGameVersionRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<Result<domain::GameVersion>>
PgGameVersionRepository::create(domain::NewGameVersion version) const {
    const auto rows = co_await database_->execSqlCoro(std::string(R"(
            INSERT INTO game_versions
                (game_id, semver, version_major, version_minor, version_patch,
                 stage, release_notes, published_at)
            VALUES ($1::uuid, $2, $3, $4, $5, $6::build_stage, $7,
                    CASE WHEN $8::boolean THEN now() ELSE NULL END)
            ON CONFLICT (game_id, semver) DO NOTHING
            RETURNING )") + VERSION_COLUMNS,
                                                      version.gameId,
                                                      version.version.text,
                                                      number(version.version.major),
                                                      number(version.version.minor),
                                                      number(version.version.patch),
                                                      std::string(domain::toString(version.stage)),
                                                      version.releaseNotes,
                                                      version.publish);

    if (rows.empty()) {
        co_return Result<domain::GameVersion>::failure(
            ErrorCode::Conflict, "this game already has a version " + version.version.text);
    }
    co_return Result<domain::GameVersion>::success(mapVersion(rows[0]));
}

drogon::Task<std::optional<domain::GameVersion>>
PgGameVersionRepository::findById(std::string id) const {
    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + VERSION_COLUMNS + " FROM game_versions WHERE id = $1::uuid", id);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapVersion(rows[0]);
}

drogon::Task<std::vector<domain::GameVersion>>
PgGameVersionRepository::listForGame(std::string gameId, bool includeUnpublished) const {
    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + VERSION_COLUMNS +
            " FROM game_versions"
            " WHERE game_id = $1::uuid AND (published_at IS NOT NULL OR $2::boolean)"
            " ORDER BY version_major DESC, version_minor DESC, version_patch DESC,"
            "          created_at DESC",
        gameId,
        includeUnpublished);

    std::vector<domain::GameVersion> versions;
    versions.reserve(rows.size());
    for (const auto& row : rows) {
        versions.push_back(mapVersion(row));
    }
    co_return versions;
}

drogon::Task<bool> PgGameVersionRepository::publish(std::string id) const {
    // COALESCE keeps the original publication timestamp when a version is published twice.
    const auto rows = co_await database_->execSqlCoro(
        "UPDATE game_versions SET published_at = COALESCE(published_at, now()) "
        "WHERE id = $1::uuid RETURNING id",
        id);

    co_return !rows.empty();
}

drogon::Task<bool> PgGameVersionRepository::remove(std::string id) const {
    const auto rows = co_await database_->execSqlCoro(
        "DELETE FROM game_versions WHERE id = $1::uuid RETURNING id", id);

    co_return !rows.empty();
}

// ---------------------------------------------------------------------------
// Builds
// ---------------------------------------------------------------------------

PgBuildRepository::PgBuildRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<Result<domain::Build>> PgBuildRepository::create(domain::NewBuild build) const {
    const auto rows =
        co_await database_->execSqlCoro(std::string(R"(
            INSERT INTO builds (game_version_id, platform, architecture)
            VALUES ($1::uuid, $2::build_platform, $3::build_architecture)
            ON CONFLICT (game_version_id, platform, architecture) DO NOTHING
            RETURNING )") + BUILD_COLUMNS,
                                        build.gameVersionId,
                                        std::string(domain::toString(build.platform)),
                                        std::string(domain::toString(build.architecture)));

    if (rows.empty()) {
        co_return Result<domain::Build>::failure(
            ErrorCode::Conflict, "this version already has a build for that platform");
    }
    co_return Result<domain::Build>::success(mapBuild(rows[0]));
}

drogon::Task<std::optional<domain::Build>> PgBuildRepository::findById(std::string id) const {
    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + BUILD_COLUMNS + " FROM builds WHERE id = $1::uuid", id);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapBuild(rows[0]);
}

drogon::Task<std::vector<domain::Build>>
PgBuildRepository::listForVersions(std::vector<std::string> versionIds) const {
    std::vector<domain::Build> builds;
    if (versionIds.empty()) {
        co_return builds;
    }

    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + BUILD_COLUMNS +
            " FROM builds"
            " WHERE game_version_id IN ("
            "     SELECT t.value::uuid FROM jsonb_array_elements_text($1::jsonb) AS t(value))"
            " ORDER BY created_at DESC",
        jsonArrayParameter(versionIds));

    builds.reserve(rows.size());
    for (const auto& row : rows) {
        builds.push_back(mapBuild(row));
    }
    co_return builds;
}

drogon::Task<std::optional<domain::BuildOwnership>>
PgBuildRepository::findOwnership(std::string buildId) const {
    const auto rows = co_await database_->execSqlCoro(
        R"(SELECT b.id AS build_id, b.game_version_id, v.game_id, g.publisher_user_id,
                  g.visibility::text AS visibility, b.status::text AS status
           FROM builds b
           JOIN game_versions v ON v.id = b.game_version_id
           JOIN games g ON g.id = v.game_id
           WHERE b.id = $1::uuid)",
        buildId);

    if (rows.empty()) {
        co_return std::nullopt;
    }

    domain::BuildOwnership ownership;
    ownership.buildId = rows[0]["build_id"].as<std::string>();
    ownership.gameVersionId = rows[0]["game_version_id"].as<std::string>();
    ownership.gameId = rows[0]["game_id"].as<std::string>();
    ownership.publisherUserId = rows[0]["publisher_user_id"].as<std::string>();
    ownership.visibility = domain::parseGameVisibility(rows[0]["visibility"].as<std::string>())
                               .value_or(domain::GameVisibility::Draft);
    ownership.status = domain::parseBuildStatus(rows[0]["status"].as<std::string>())
                           .value_or(domain::BuildStatus::Uploading);
    co_return ownership;
}

drogon::Task<std::vector<domain::ManifestEntry>>
PgBuildRepository::filesFor(std::string buildId) const {
    const auto rows = co_await database_->execSqlCoro(
        "SELECT bf.relative_path, bf.blob_sha256, bf.is_executable, b.size_bytes "
        "FROM build_files bf JOIN blobs b ON b.sha256 = bf.blob_sha256 "
        "WHERE bf.build_id = $1::uuid ORDER BY bf.relative_path",
        buildId);

    std::vector<domain::ManifestEntry> files;
    files.reserve(rows.size());
    for (const auto& row : rows) {
        domain::ManifestEntry entry;
        entry.relativePath = row["relative_path"].as<std::string>();
        entry.blobSha256 = row["blob_sha256"].as<std::string>();
        entry.sizeBytes = row["size_bytes"].as<int64_t>();
        entry.isExecutable = row["is_executable"].as<bool>();
        files.push_back(std::move(entry));
    }
    co_return files;
}

drogon::Task<std::optional<domain::Build>>
PgBuildRepository::finalize(std::string buildId, FinalizedManifest manifest) const {
    Json::Value files(Json::arrayValue);
    for (const auto& file : manifest.files) {
        Json::Value entry;
        entry["path"] = file.relativePath;
        entry["sha256"] = file.blobSha256;
        entry["executable"] = file.isExecutable;
        files.append(entry);
    }

    // One statement, for two reasons. A Drogon transaction commits asynchronously when its
    // object is destroyed, so a build could be reported ready before its rows are durable;
    // and gating the insert on the update's result is what makes a second, concurrent
    // finalize a no-op instead of a duplicate-key error.
    const auto rows =
        co_await database_->execSqlCoro(std::string(R"(
            WITH updated AS (
                UPDATE builds SET
                    status = 'ready',
                    manifest_sha256 = $2,
                    total_size_bytes = $3,
                    file_count = $4,
                    entrypoint_relative_path = $5,
                    default_launch_args = $6,
                    ready_at = now()
                WHERE id = $1::uuid AND status = 'uploading'
                RETURNING *
            ), inserted AS (
                INSERT INTO build_files (build_id, relative_path, blob_sha256, is_executable)
                SELECT u.id, t.entry->>'path', t.entry->>'sha256',
                       (t.entry->>'executable')::boolean
                FROM updated u, jsonb_array_elements($7::jsonb) AS t(entry)
            )
            SELECT )") + BUILD_COLUMNS + " FROM updated",
                                        buildId,
                                        manifest.manifestSha256,
                                        number(manifest.totalSizeBytes),
                                        number(static_cast<int64_t>(manifest.files.size())),
                                        manifest.entrypointRelativePath,
                                        manifest.defaultLaunchArgs,
                                        toCompactJson(files));

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapBuild(rows[0]);
}

drogon::Task<bool> PgBuildRepository::markFailed(std::string buildId) const {
    const auto rows = co_await database_->execSqlCoro(
        "UPDATE builds SET status = 'failed' WHERE id = $1::uuid AND status <> 'ready' "
        "RETURNING id",
        buildId);

    co_return !rows.empty();
}

drogon::Task<bool> PgBuildRepository::remove(std::string buildId) const {
    // build_files goes with it by cascade; the blobs those rows named are left alone, because
    // another build may still reference them and answering that is the collector's job.
    const auto rows = co_await database_->execSqlCoro(
        "DELETE FROM builds WHERE id = $1::uuid RETURNING id", buildId);

    co_return !rows.empty();
}

// ---------------------------------------------------------------------------
// Media
// ---------------------------------------------------------------------------

PgMediaRepository::PgMediaRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<Result<IMediaRepository::Stored>>
PgMediaRepository::create(domain::NewGameMedia media) const {
    // One statement, for the reason D15 gives about upload offsets: a publisher replacing a
    // cover must never observe a game with none, and a read-then-write leaves exactly that
    // window. `previous` reads the pre-command snapshot, so it names the file the upsert is
    // about to displace even though RETURNING can only describe the row that survives.
    const auto rows = co_await database_->execSqlCoro(
        std::string(R"(
            WITH previous AS (
                SELECT storage_key FROM game_media
                WHERE game_id = $1::uuid AND kind = $2::game_media_kind AND kind <> 'screenshot'
            ),
            upserted AS (
                INSERT INTO game_media
                    (game_id, kind, storage_key, sha256, content_type, size_bytes,
                     alt_text, sort_order)
                VALUES ($1::uuid, $2::game_media_kind, $3, $4, $5, $6, $7, $8)
                ON CONFLICT (game_id, kind) WHERE kind <> 'screenshot'
                DO UPDATE SET storage_key  = excluded.storage_key,
                              sha256       = excluded.sha256,
                              content_type = excluded.content_type,
                              size_bytes   = excluded.size_bytes,
                              alt_text     = excluded.alt_text,
                              sort_order   = excluded.sort_order
                RETURNING *
            )
            SELECT )") +
            MEDIA_COLUMNS +
            R"(, COALESCE((SELECT storage_key FROM previous), '') AS replaced_storage_key
               FROM upserted m)",
        media.gameId,
        std::string(domain::toString(media.kind)),
        media.storageKey,
        media.sha256,
        media.contentType,
        number(media.sizeBytes),
        media.altText,
        number(media.sortOrder));

    if (rows.empty()) {
        co_return Result<Stored>::failure(ErrorCode::NotFound, "no such game");
    }

    Stored stored;
    stored.media = mapMedia(rows[0]);
    stored.replacedStorageKey = rows[0]["replaced_storage_key"].as<std::string>();
    // Replacing an image with itself is a no-op on disk, and reporting the key as displaced
    // would make the caller delete the file the surviving row points at.
    if (stored.replacedStorageKey == stored.media.storageKey) {
        stored.replacedStorageKey.clear();
    }
    co_return Result<Stored>::success(std::move(stored));
}

drogon::Task<std::vector<domain::GameMedia>>
PgMediaRepository::listForGame(std::string gameId) const {
    const auto rows =
        co_await database_->execSqlCoro(std::string("SELECT ") + MEDIA_COLUMNS +
                                            R"( FROM game_media m WHERE m.game_id = $1::uuid
                ORDER BY m.kind, m.sort_order, m.created_at, m.id)",
                                        gameId);

    std::vector<domain::GameMedia> media;
    media.reserve(rows.size());
    for (const auto& row : rows) {
        media.push_back(mapMedia(row));
    }
    co_return media;
}

drogon::Task<std::optional<domain::GameMedia>> PgMediaRepository::findById(std::string id) const {
    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + MEDIA_COLUMNS + " FROM game_media m WHERE m.id = $1::uuid", id);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapMedia(rows[0]);
}

drogon::Task<std::optional<domain::GameMedia>>
PgMediaRepository::update(std::string id, domain::GameMediaUpdate changes) const {
    std::optional<std::string> sortOrder;
    if (changes.sortOrder.has_value()) {
        sortOrder = number(*changes.sortOrder);
    }

    const auto rows = co_await database_->execSqlCoro(std::string(R"(
            WITH updated AS (
                UPDATE game_media SET
                    alt_text   = COALESCE($2, alt_text),
                    sort_order = COALESCE($3::integer, sort_order)
                WHERE id = $1::uuid
                RETURNING *
            )
            SELECT )") + MEDIA_COLUMNS + " FROM updated m",
                                                      id,
                                                      changes.altText,
                                                      sortOrder);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapMedia(rows[0]);
}

drogon::Task<std::optional<domain::GameMedia>> PgMediaRepository::remove(std::string id) const {
    const auto rows = co_await database_->execSqlCoro(std::string(R"(
            WITH deleted AS (
                DELETE FROM game_media WHERE id = $1::uuid RETURNING *
            )
            SELECT )") + MEDIA_COLUMNS + " FROM deleted m",
                                                      id);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapMedia(rows[0]);
}

drogon::Task<int> PgMediaRepository::countForGame(std::string gameId,
                                                  domain::MediaKind kind) const {
    const auto rows =
        co_await database_->execSqlCoro("SELECT count(*) AS total FROM game_media "
                                        "WHERE game_id = $1::uuid AND kind = $2::game_media_kind",
                                        gameId,
                                        std::string(domain::toString(kind)));

    co_return static_cast<int>(rows[0]["total"].as<int64_t>());
}

drogon::Task<bool> PgMediaRepository::isStorageKeyReferenced(std::string storageKey) const {
    const auto rows = co_await database_->execSqlCoro(
        "SELECT 1 FROM game_media WHERE storage_key = $1 LIMIT 1", storageKey);

    co_return !rows.empty();
}

// ---------------------------------------------------------------------------
// Patch notes
// ---------------------------------------------------------------------------

PgPatchNoteRepository::PgPatchNoteRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<Result<domain::PatchNote>>
PgPatchNoteRepository::create(domain::NewPatchNote note) const {
    // The author's display name comes from a join, which INSERT ... RETURNING cannot do, so
    // the insert feeds a CTE the outer query joins as usual - the same shape games use.
    const auto rows = co_await database_->execSqlCoro(
        std::string(R"(
            WITH inserted AS (
                INSERT INTO patch_notes
                    (game_id, game_version_id, title, body_markdown, author_user_id, published_at)
                VALUES ($1::uuid, NULLIF($2, '')::uuid, $3, $4, $5::uuid,
                        CASE WHEN $6::boolean THEN now() ELSE NULL END)
                RETURNING *
            )
            SELECT )") +
            PATCH_NOTE_COLUMNS + " FROM inserted n LEFT JOIN users a ON a.id = n.author_user_id",
        note.gameId,
        note.gameVersionId,
        note.title,
        note.bodyMarkdown,
        note.authorUserId,
        note.publish ? "true" : "false");

    if (rows.empty()) {
        co_return Result<domain::PatchNote>::failure(ErrorCode::NotFound, "no such game");
    }
    co_return Result<domain::PatchNote>::success(mapPatchNote(rows[0]));
}

drogon::Task<std::optional<domain::PatchNote>>
PgPatchNoteRepository::findById(std::string id) const {
    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + PATCH_NOTE_COLUMNS + PATCH_NOTE_SOURCE + " WHERE n.id = $1::uuid",
        id);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapPatchNote(rows[0]);
}

drogon::Task<PatchNotePage> PgPatchNoteRepository::search(PatchNoteQuery query) const {
    const std::string filter =
        " WHERE n.game_id = $1::uuid AND ($2::boolean OR n.published_at IS NOT NULL) ";

    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + PATCH_NOTE_COLUMNS + PATCH_NOTE_SOURCE + filter +
            // Newest first, and a draft has no publication date, so it is ordered by when it
            // was written instead of sinking to the end of the publisher's own list.
            " ORDER BY COALESCE(n.published_at, n.created_at) DESC, n.id LIMIT $3 OFFSET $4",
        query.gameId,
        query.includeUnpublished ? "true" : "false",
        number(query.limit),
        number(query.offset));

    PatchNotePage page;
    page.items.reserve(rows.size());
    for (const auto& row : rows) {
        page.items.push_back(mapPatchNote(row));
    }

    const auto totals = co_await database_->execSqlCoro(
        std::string("SELECT count(*) AS total FROM patch_notes n ") + filter,
        query.gameId,
        query.includeUnpublished ? "true" : "false");
    page.total = totals[0]["total"].as<int64_t>();
    co_return page;
}

drogon::Task<std::optional<domain::PatchNote>>
PgPatchNoteRepository::update(std::string id, domain::PatchNoteUpdate changes) const {
    std::optional<std::string> published;
    if (changes.published.has_value()) {
        published = *changes.published ? "true" : "false";
    }

    const auto rows = co_await database_->execSqlCoro(
        std::string(R"(
            WITH updated AS (
                UPDATE patch_notes SET
                    title           = COALESCE($2, title),
                    body_markdown   = COALESCE($3, body_markdown),
                    game_version_id = CASE WHEN $4::text IS NULL THEN game_version_id
                                           WHEN $4 = '' THEN NULL
                                           ELSE $4::uuid END,
                    published_at    = CASE WHEN $5::boolean IS NULL THEN published_at
                                           WHEN $5::boolean THEN COALESCE(published_at, now())
                                           ELSE NULL END
                WHERE id = $1::uuid
                RETURNING *
            )
            SELECT )") +
            PATCH_NOTE_COLUMNS + " FROM updated n LEFT JOIN users a ON a.id = n.author_user_id",
        id,
        changes.title,
        changes.bodyMarkdown,
        changes.gameVersionId,
        published);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return mapPatchNote(rows[0]);
}

drogon::Task<bool> PgPatchNoteRepository::remove(std::string id) const {
    const auto rows = co_await database_->execSqlCoro(
        "DELETE FROM patch_notes WHERE id = $1::uuid RETURNING id", id);

    co_return !rows.empty();
}

// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------

PgLibraryRepository::PgLibraryRepository(drogon::orm::DbClientPtr database)
    : database_(std::move(database)) {}

drogon::Task<bool> PgLibraryRepository::add(std::string userId, std::string gameId) const {
    // Selecting the game id rather than passing it straight through makes the insert a no-op
    // for a game that does not exist, which the EXISTS below then reports as false.
    const auto rows = co_await database_->execSqlCoro(
        R"(WITH inserted AS (
               INSERT INTO user_games (user_id, game_id)
               SELECT $1::uuid, g.id FROM games g WHERE g.id = $2::uuid
               ON CONFLICT (user_id, game_id) DO NOTHING
               RETURNING game_id
           )
           SELECT EXISTS (SELECT 1 FROM inserted)
               OR EXISTS (SELECT 1 FROM user_games
                          WHERE user_id = $1::uuid AND game_id = $2::uuid) AS present)",
        userId,
        gameId);

    co_return !rows.empty() && rows[0]["present"].as<bool>();
}

drogon::Task<bool> PgLibraryRepository::remove(std::string userId, std::string gameId) const {
    const auto rows = co_await database_->execSqlCoro(
        "DELETE FROM user_games WHERE user_id = $1::uuid AND game_id = $2::uuid RETURNING game_id",
        userId,
        gameId);

    co_return !rows.empty();
}

drogon::Task<bool> PgLibraryRepository::contains(std::string userId, std::string gameId) const {
    const auto rows = co_await database_->execSqlCoro(
        "SELECT 1 FROM user_games WHERE user_id = $1::uuid AND game_id = $2::uuid", userId, gameId);

    co_return !rows.empty();
}

drogon::Task<std::vector<domain::Game>> PgLibraryRepository::list(std::string userId) const {
    const auto rows = co_await database_->execSqlCoro(
        std::string("SELECT ") + GAME_COLUMNS +
            " FROM user_games ug"
            " JOIN games g ON g.id = ug.game_id"
            " JOIN users u ON u.id = g.publisher_user_id"
            " WHERE ug.user_id = $1::uuid"
            " ORDER BY g.release_date DESC NULLS LAST, g.created_at DESC, g.id",
        userId);

    std::vector<domain::Game> games;
    games.reserve(rows.size());
    for (const auto& row : rows) {
        games.push_back(mapGame(row));
    }
    co_return games;
}

} // namespace launcher::repositories::postgres
