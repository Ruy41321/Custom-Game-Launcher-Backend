#pragma once

#include <algorithm>
#include <string>
#include <string_view>

#include "domain/Actor.h"

namespace launcher::domain {

/// Whether a permission key belongs to the administrative surface.
///
/// The prefix is the rule rather than a fixed list, so a permission a later migration adds to
/// the `admin` role is administrative without a code change here — the same reason the role
/// graph is table driven in the first place.
inline bool isAdministrativePermission(std::string_view permission) {
    constexpr std::string_view PREFIX = "admin.";
    return permission.size() > PREFIX.size() && permission.substr(0, PREFIX.size()) == PREFIX;
}

/// Whether an actor may hold a session on the administrative surface at all.
///
/// Deliberately coarse: it decides whether signing in is possible, not what the session may
/// then do. Every individual action still checks the permission it needs, so an operator
/// holding only `admin.users.manage` signs in and is refused everywhere else.
inline bool mayUseAdminSurface(const Actor& actor) {
    return std::any_of(actor.permissions.begin(),
                       actor.permissions.end(),
                       [](const std::string& held) { return isAdministrativePermission(held); });
}

} // namespace launcher::domain
