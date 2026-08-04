#pragma once

#include <drogon/utils/coroutine.h>

#include <optional>
#include <string>
#include <vector>

#include "common/Result.h"
#include "domain/Media.h"

namespace launcher::repositories {

/// Persistence for game artwork. Every parameter is taken by value — a coroutine's reference
/// parameters dangle as soon as it first suspends.
class IMediaRepository {
  public:
    virtual ~IMediaRepository() = default;

    /// Inserts, or replaces the existing row when the kind is a singleton.
    ///
    /// Replacement happens in the same statement as the insert, so a publisher uploading a new
    /// cover never observes a game with none. The row that was displaced is returned alongside
    /// the new one, because the file it pointed at may now be unreferenced.
    struct Stored {
        domain::GameMedia media;
        /// Storage key the replaced row held, empty when nothing was replaced.
        std::string replacedStorageKey;
    };

    virtual drogon::Task<common::Result<Stored>> create(domain::NewGameMedia media) const = 0;

    virtual drogon::Task<std::vector<domain::GameMedia>> listForGame(std::string gameId) const = 0;

    virtual drogon::Task<std::optional<domain::GameMedia>> findById(std::string id) const = 0;

    virtual drogon::Task<std::optional<domain::GameMedia>>
    update(std::string id, domain::GameMediaUpdate changes) const = 0;

    /// Returns the row that was removed, so its storage key can be considered for collection.
    virtual drogon::Task<std::optional<domain::GameMedia>> remove(std::string id) const = 0;

    virtual drogon::Task<int> countForGame(std::string gameId, domain::MediaKind kind) const = 0;

    /// Whether any row still points at a file. A storage key is shared by every game whose
    /// artwork hashes the same, so deleting a row is not the same as deleting a file.
    virtual drogon::Task<bool> isStorageKeyReferenced(std::string storageKey) const = 0;
};

} // namespace launcher::repositories
