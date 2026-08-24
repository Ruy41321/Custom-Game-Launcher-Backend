#pragma once

#include <drogon/utils/coroutine.h>

#include <optional>
#include <string>
#include <vector>

#include "common/Result.h"
#include "domain/Catalog.h"

namespace launcher::repositories {

class IGameVersionRepository {
  public:
    virtual ~IGameVersionRepository() = default;

    /// Conflict when the game already has that semver.
    virtual drogon::Task<common::Result<domain::GameVersion>>
    create(domain::NewGameVersion version) const = 0;

    virtual drogon::Task<std::optional<domain::GameVersion>> findById(std::string id) const = 0;

    /// Ordered newest first by numeric version components, never by the semver text.
    virtual drogon::Task<std::vector<domain::GameVersion>>
    listForGame(std::string gameId, bool includeUnpublished) const = 0;

    /// Applies a partial update, the shape `IGameRepository::update` has for a game. Nullopt
    /// when no such version exists.
    ///
    /// Publishing is idempotent: a version published twice keeps its original timestamp, so
    /// the date a release went out cannot be moved by pressing a button again. This replaced a
    /// `publish(id)` that had existed since migration 0001 and that **nothing ever called** —
    /// the ability was in the repository the whole time and no route reached it.
    virtual drogon::Task<std::optional<domain::GameVersion>>
    update(std::string id, domain::GameVersionUpdate changes) const = 0;

    /// Deletes a version and cascades to its builds. Nothing is done about the blobs those
    /// builds referenced; the collector notices them on its next pass.
    virtual drogon::Task<bool> remove(std::string id) const = 0;
};

} // namespace launcher::repositories
