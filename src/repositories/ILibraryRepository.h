#pragma once

#include <drogon/utils/coroutine.h>

#include <string>
#include <vector>

#include "domain/Catalog.h"

namespace launcher::repositories {

/// Server-side library membership. What is *installed* is per machine and stays on the
/// client; this only records which games the account has added.
class ILibraryRepository {
  public:
    virtual ~ILibraryRepository() = default;

    /// Idempotent. False when the game does not exist.
    virtual drogon::Task<bool> add(std::string userId, std::string gameId) const = 0;

    /// False when the game was not in the library.
    virtual drogon::Task<bool> remove(std::string userId, std::string gameId) const = 0;

    virtual drogon::Task<bool> contains(std::string userId, std::string gameId) const = 0;

    virtual drogon::Task<std::vector<domain::Game>> list(std::string userId) const = 0;
};

} // namespace launcher::repositories
