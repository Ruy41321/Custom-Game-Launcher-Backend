#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/Result.h"
#include "domain/Catalog.h"

namespace launcher::repositories {

/// How the catalog is ordered when a caller does not care to say.
enum class GameSort {
    ReleaseDate, ///< default for both the library and Explore
    Title,
    RecentlyAdded,
};

inline constexpr int DEFAULT_GAME_PAGE_SIZE = 20;
inline constexpr int MAX_GAME_PAGE_SIZE = 100;

struct GameQuery {
    std::string search; ///< matched against the title, empty means all
    GameSort sort{GameSort::ReleaseDate};
    int limit{DEFAULT_GAME_PAGE_SIZE};
    int offset{0};
    /// Restricts the result to one publisher. Empty means every publisher.
    std::string publisherUserId;
    /// Explore only ever shows published games; the publisher's own dashboard sets this so a
    /// draft is visible to the account that owns it.
    bool includeUnpublished{false};
};

struct GamePage {
    std::vector<domain::Game> items;
    int64_t total{0};
};

/// Persistence for the catalog. Every parameter is taken by value — a coroutine's reference
/// parameters dangle as soon as it first suspends.
class IGameRepository {
  public:
    virtual ~IGameRepository() = default;

    virtual drogon::Task<std::optional<domain::Game>> findById(std::string id) const = 0;

    virtual drogon::Task<std::optional<domain::Game>> findBySlug(std::string slug) const = 0;

    /// Conflict when the slug is taken. Decided by the unique index rather than a preceding
    /// SELECT, so two simultaneous creations cannot both succeed.
    virtual drogon::Task<common::Result<domain::Game>> create(domain::NewGame game) const = 0;

    /// Nullopt when no game has that id. Absent fields of the update are left untouched.
    virtual drogon::Task<std::optional<domain::Game>> update(std::string id,
                                                             domain::GameUpdate changes) const = 0;

    virtual drogon::Task<GamePage> search(GameQuery query) const = 0;
};

} // namespace launcher::repositories
