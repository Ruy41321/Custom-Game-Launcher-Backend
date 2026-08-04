#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "common/Random.h"
#include "repositories/IBuildRepository.h"
#include "repositories/IGameRepository.h"
#include "repositories/IGameVersionRepository.h"
#include "repositories/ILibraryRepository.h"
#include "repositories/IMediaRepository.h"

namespace launcher::testing {

/// In-memory doubles for the catalog repositories.
///
/// Hand-written for the same reason as the authentication fakes: the interfaces return
/// coroutines, and the tests only mean something if the doubles model the behaviour the
/// service relies on — unique slugs, ownership, offsets that only advance when they match.
///
/// The interface methods are const because callers only read through them, so the recorded
/// state is mutable.

class FakeGameRepository : public repositories::IGameRepository {
  public:
    mutable std::vector<domain::Game> games;

    domain::Game seed(domain::Game game) const {
        if (game.id.empty()) {
            game.id = common::randomUuid();
        }
        if (game.publisherDisplayName.empty()) {
            game.publisherDisplayName = "Publisher";
        }
        games.push_back(game);
        return game;
    }

    drogon::Task<std::optional<domain::Game>> findById(std::string id) const override {
        for (const auto& game : games) {
            if (game.id == id) {
                co_return game;
            }
        }
        co_return std::nullopt;
    }

    drogon::Task<std::optional<domain::Game>> findBySlug(std::string slug) const override {
        for (const auto& game : games) {
            if (game.slug == slug) {
                co_return game;
            }
        }
        co_return std::nullopt;
    }

    drogon::Task<common::Result<domain::Game>> create(domain::NewGame candidate) const override {
        for (const auto& game : games) {
            if (game.slug == candidate.slug) {
                co_return common::Result<domain::Game>::failure(
                    common::ErrorCode::Conflict, "a game with that slug already exists");
            }
        }

        domain::Game created;
        created.id = common::randomUuid();
        created.slug = candidate.slug;
        created.title = candidate.title;
        created.summary = candidate.summary;
        created.description = candidate.description;
        created.publisherUserId = candidate.publisherUserId;
        created.publisherDisplayName = "Publisher";
        created.releaseDate = candidate.releaseDate;
        created.visibility = candidate.visibility;
        games.push_back(created);
        co_return common::Result<domain::Game>::success(created);
    }

    drogon::Task<std::optional<domain::Game>> update(std::string id,
                                                     domain::GameUpdate changes) const override {
        for (auto& game : games) {
            if (game.id != id) {
                continue;
            }
            if (changes.title) {
                game.title = *changes.title;
            }
            if (changes.summary) {
                game.summary = *changes.summary;
            }
            if (changes.description) {
                game.description = *changes.description;
            }
            if (changes.releaseDate) {
                game.releaseDate = *changes.releaseDate;
            }
            if (changes.visibility) {
                game.visibility = *changes.visibility;
            }
            co_return game;
        }
        co_return std::nullopt;
    }

    drogon::Task<repositories::GamePage> search(repositories::GameQuery query) const override {
        std::vector<domain::Game> matched;
        for (const auto& game : games) {
            if (!query.includeUnpublished && game.visibility != domain::GameVisibility::Public) {
                continue;
            }
            if (!query.publisherUserId.empty() && game.publisherUserId != query.publisherUserId) {
                continue;
            }
            if (!query.search.empty() && game.title.find(query.search) == std::string::npos) {
                continue;
            }
            matched.push_back(game);
        }

        repositories::GamePage page;
        page.total = static_cast<int64_t>(matched.size());

        const auto begin =
            std::min<std::size_t>(static_cast<std::size_t>(query.offset), matched.size());
        const auto end =
            std::min<std::size_t>(begin + static_cast<std::size_t>(query.limit), matched.size());
        page.items.assign(matched.begin() + static_cast<std::ptrdiff_t>(begin),
                          matched.begin() + static_cast<std::ptrdiff_t>(end));
        co_return page;
    }
};

class FakeGameVersionRepository : public repositories::IGameVersionRepository {
  public:
    mutable std::vector<domain::GameVersion> versions;

    domain::GameVersion seed(domain::GameVersion version) const {
        if (version.id.empty()) {
            version.id = common::randomUuid();
        }
        versions.push_back(version);
        return version;
    }

    drogon::Task<common::Result<domain::GameVersion>>
    create(domain::NewGameVersion candidate) const override {
        for (const auto& version : versions) {
            if (version.gameId == candidate.gameId && version.semver == candidate.version.text) {
                co_return common::Result<domain::GameVersion>::failure(
                    common::ErrorCode::Conflict, "this game already has that version");
            }
        }

        domain::GameVersion created;
        created.id = common::randomUuid();
        created.gameId = candidate.gameId;
        created.semver = candidate.version.text;
        created.versionMajor = candidate.version.major;
        created.versionMinor = candidate.version.minor;
        created.versionPatch = candidate.version.patch;
        created.stage = candidate.stage;
        created.releaseNotes = candidate.releaseNotes;
        created.publishedAt = candidate.publish ? "2026-01-01T00:00:00Z" : "";
        versions.push_back(created);
        co_return common::Result<domain::GameVersion>::success(created);
    }

    drogon::Task<std::optional<domain::GameVersion>> findById(std::string id) const override {
        for (const auto& version : versions) {
            if (version.id == id) {
                co_return version;
            }
        }
        co_return std::nullopt;
    }

    drogon::Task<std::vector<domain::GameVersion>>
    listForGame(std::string gameId, bool includeUnpublished) const override {
        std::vector<domain::GameVersion> matched;
        for (const auto& version : versions) {
            if (version.gameId != gameId) {
                continue;
            }
            if (!includeUnpublished && version.publishedAt.empty()) {
                continue;
            }
            matched.push_back(version);
        }
        co_return matched;
    }

    drogon::Task<bool> remove(std::string id) const override {
        const auto before = versions.size();
        versions.erase(std::remove_if(versions.begin(),
                                      versions.end(),
                                      [&](const auto& version) { return version.id == id; }),
                       versions.end());
        co_return versions.size() != before;
    }

    drogon::Task<bool> publish(std::string id) const override {
        for (auto& version : versions) {
            if (version.id == id) {
                if (version.publishedAt.empty()) {
                    version.publishedAt = "2026-01-01T00:00:00Z";
                }
                co_return true;
            }
        }
        co_return false;
    }
};

class FakeBuildRepository : public repositories::IBuildRepository {
  public:
    mutable std::vector<domain::Build> builds;
    mutable std::vector<domain::BuildOwnership> ownerships;
    mutable std::map<std::string, std::vector<domain::ManifestEntry>> files;

    domain::Build seed(domain::Build build, domain::BuildOwnership ownership) const {
        if (build.id.empty()) {
            build.id = common::randomUuid();
        }
        ownership.buildId = build.id;
        ownership.gameVersionId = build.gameVersionId;
        ownership.status = build.status;
        builds.push_back(build);
        ownerships.push_back(ownership);
        return build;
    }

    drogon::Task<common::Result<domain::Build>> create(domain::NewBuild candidate) const override {
        for (const auto& build : builds) {
            if (build.gameVersionId == candidate.gameVersionId &&
                build.platform == candidate.platform &&
                build.architecture == candidate.architecture) {
                co_return common::Result<domain::Build>::failure(
                    common::ErrorCode::Conflict, "this version already has that build");
            }
        }

        domain::Build created;
        created.id = common::randomUuid();
        created.gameVersionId = candidate.gameVersionId;
        created.platform = candidate.platform;
        created.architecture = candidate.architecture;
        created.status = domain::BuildStatus::Uploading;
        builds.push_back(created);
        co_return common::Result<domain::Build>::success(created);
    }

    drogon::Task<std::optional<domain::Build>> findById(std::string id) const override {
        for (const auto& build : builds) {
            if (build.id == id) {
                co_return build;
            }
        }
        co_return std::nullopt;
    }

    drogon::Task<std::vector<domain::Build>>
    listForVersions(std::vector<std::string> versionIds) const override {
        std::vector<domain::Build> matched;
        for (const auto& build : builds) {
            if (std::find(versionIds.begin(), versionIds.end(), build.gameVersionId) !=
                versionIds.end()) {
                matched.push_back(build);
            }
        }
        co_return matched;
    }

    drogon::Task<std::optional<domain::BuildOwnership>>
    findOwnership(std::string buildId) const override {
        for (const auto& ownership : ownerships) {
            if (ownership.buildId == buildId) {
                co_return ownership;
            }
        }
        co_return std::nullopt;
    }

    drogon::Task<std::vector<domain::ManifestEntry>> filesFor(std::string buildId) const override {
        const auto found = files.find(buildId);
        co_return found == files.end() ? std::vector<domain::ManifestEntry>{} : found->second;
    }

    drogon::Task<std::optional<domain::Build>>
    finalize(std::string buildId, repositories::FinalizedManifest manifest) const override {
        for (auto& build : builds) {
            if (build.id != buildId || build.status != domain::BuildStatus::Uploading) {
                continue;
            }

            build.status = domain::BuildStatus::Ready;
            build.manifestSha256 = manifest.manifestSha256;
            build.totalSizeBytes = manifest.totalSizeBytes;
            build.fileCount = static_cast<int32_t>(manifest.files.size());
            build.entrypointRelativePath = manifest.entrypointRelativePath;
            build.defaultLaunchArgs = manifest.defaultLaunchArgs;
            build.readyAt = "2026-01-01T00:00:00Z";

            files[buildId] = manifest.files;
            for (auto& ownership : ownerships) {
                if (ownership.buildId == buildId) {
                    ownership.status = domain::BuildStatus::Ready;
                }
            }
            co_return build;
        }
        co_return std::nullopt;
    }

    drogon::Task<bool> markFailed(std::string buildId) const override {
        for (auto& build : builds) {
            if (build.id == buildId && build.status != domain::BuildStatus::Ready) {
                build.status = domain::BuildStatus::Failed;
                co_return true;
            }
        }
        co_return false;
    }

    drogon::Task<bool> remove(std::string buildId) const override {
        const auto before = builds.size();
        builds.erase(std::remove_if(builds.begin(),
                                    builds.end(),
                                    [&](const auto& build) { return build.id == buildId; }),
                     builds.end());
        ownerships.erase(
            std::remove_if(ownerships.begin(),
                           ownerships.end(),
                           [&](const auto& owned) { return owned.buildId == buildId; }),
            ownerships.end());
        files.erase(buildId);
        co_return builds.size() != before;
    }
};

class FakeLibraryRepository : public repositories::ILibraryRepository {
  public:
    /// Set by tests so `add` can refuse a game that does not exist, as the real query does.
    mutable std::vector<std::string> knownGameIds;
    mutable std::map<std::string, std::vector<domain::Game>> gamesByUser;
    mutable std::vector<std::pair<std::string, std::string>> memberships;

    drogon::Task<bool> add(std::string userId, std::string gameId) const override {
        if (!knownGameIds.empty() &&
            std::find(knownGameIds.begin(), knownGameIds.end(), gameId) == knownGameIds.end()) {
            co_return false;
        }
        if (!co_await contains(userId, gameId)) {
            memberships.emplace_back(userId, gameId);
        }
        co_return true;
    }

    drogon::Task<bool> remove(std::string userId, std::string gameId) const override {
        const auto before = memberships.size();
        memberships.erase(
            std::remove(memberships.begin(), memberships.end(), std::make_pair(userId, gameId)),
            memberships.end());
        co_return memberships.size() != before;
    }

    drogon::Task<bool> contains(std::string userId, std::string gameId) const override {
        co_return std::find(memberships.begin(),
                            memberships.end(),
                            std::make_pair(userId, gameId)) != memberships.end();
    }

    drogon::Task<std::vector<domain::Game>> list(std::string userId) const override {
        const auto found = gamesByUser.find(userId);
        co_return found == gamesByUser.end() ? std::vector<domain::Game>{} : found->second;
    }
};

class FakeMediaRepository : public repositories::IMediaRepository {
  public:
    mutable std::vector<domain::GameMedia> media;

    domain::GameMedia seed(domain::GameMedia row) const {
        if (row.id.empty()) {
            row.id = common::randomUuid();
        }
        media.push_back(row);
        return row;
    }

    drogon::Task<common::Result<Stored>> create(domain::NewGameMedia candidate) const override {
        Stored stored;

        // Models the partial unique index: a second cover replaces the first, a screenshot
        // never does. A fake that let two covers coexist would let a service bug through.
        if (domain::isSingletonKind(candidate.kind)) {
            const auto existing = std::find_if(media.begin(), media.end(), [&](const auto& row) {
                return row.gameId == candidate.gameId && row.kind == candidate.kind;
            });
            if (existing != media.end()) {
                if (existing->storageKey != candidate.storageKey) {
                    stored.replacedStorageKey = existing->storageKey;
                }
                existing->storageKey = candidate.storageKey;
                existing->sha256 = candidate.sha256;
                existing->contentType = candidate.contentType;
                existing->sizeBytes = candidate.sizeBytes;
                existing->altText = candidate.altText;
                existing->sortOrder = candidate.sortOrder;
                stored.media = *existing;
                co_return common::Result<Stored>::success(std::move(stored));
            }
        }

        domain::GameMedia row;
        row.id = common::randomUuid();
        row.gameId = candidate.gameId;
        row.kind = candidate.kind;
        row.storageKey = candidate.storageKey;
        row.sha256 = candidate.sha256;
        row.contentType = candidate.contentType;
        row.sizeBytes = candidate.sizeBytes;
        row.altText = candidate.altText;
        row.sortOrder = candidate.sortOrder;
        row.createdAt = "2026-01-01T00:00:00Z";
        media.push_back(row);

        stored.media = row;
        co_return common::Result<Stored>::success(std::move(stored));
    }

    drogon::Task<std::vector<domain::GameMedia>> listForGame(std::string gameId) const override {
        std::vector<domain::GameMedia> found;
        for (const auto& row : media) {
            if (row.gameId == gameId) {
                found.push_back(row);
            }
        }
        co_return found;
    }

    drogon::Task<std::optional<domain::GameMedia>> findById(std::string id) const override {
        for (const auto& row : media) {
            if (row.id == id) {
                co_return row;
            }
        }
        co_return std::nullopt;
    }

    drogon::Task<std::optional<domain::GameMedia>>
    update(std::string id, domain::GameMediaUpdate changes) const override {
        for (auto& row : media) {
            if (row.id != id) {
                continue;
            }
            if (changes.altText.has_value()) {
                row.altText = *changes.altText;
            }
            if (changes.sortOrder.has_value()) {
                row.sortOrder = *changes.sortOrder;
            }
            co_return row;
        }
        co_return std::nullopt;
    }

    drogon::Task<std::optional<domain::GameMedia>> remove(std::string id) const override {
        const auto found =
            std::find_if(media.begin(), media.end(), [&](const auto& row) { return row.id == id; });
        if (found == media.end()) {
            co_return std::nullopt;
        }
        const auto removed = *found;
        media.erase(found);
        co_return removed;
    }

    drogon::Task<int> countForGame(std::string gameId, domain::MediaKind kind) const override {
        int total = 0;
        for (const auto& row : media) {
            if (row.gameId == gameId && row.kind == kind) {
                ++total;
            }
        }
        co_return total;
    }

    drogon::Task<bool> isStorageKeyReferenced(std::string storageKey) const override {
        for (const auto& row : media) {
            if (row.storageKey == storageKey) {
                co_return true;
            }
        }
        co_return false;
    }
};

} // namespace launcher::testing
