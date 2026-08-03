#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "domain/Role.h"

namespace launcher::domain {

/// Who is making a request, as the service layer sees it.
///
/// Built by the controllers from the verified access-token claims. Services take this rather
/// than the claims themselves so that business rules stay independent of how the caller was
/// authenticated — and so that unit tests can construct an actor in one line.
struct Actor {
    std::string userId;
    std::vector<std::string> permissions;

    bool can(std::string_view permission) const {
        return std::any_of(permissions.begin(), permissions.end(), [&](const std::string& held) {
            return held == permission;
        });
    }

    /// May act on games belonging to somebody else. Ownership checks in the service layer are
    /// written as "owner or this", never as "is admin" alone.
    bool managesAnyGame() const { return can(permissions::ADMIN_GAMES_MANAGE); }

    bool owns(std::string_view publisherUserId) const { return userId == publisherUserId; }
};

} // namespace launcher::domain
