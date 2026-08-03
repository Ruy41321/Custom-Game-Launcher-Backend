#pragma once

#include <string>

namespace launcher::domain {

/// Role keys seeded by migration 0001. These constants exist so application code can refer
/// to a role without repeating a string literal; the *set* of roles remains data, and a new
/// role needs no code change unless the code has to reason about it specifically.
namespace roles {
inline constexpr const char* PLAYER = "player";
inline constexpr const char* DEV = "dev";
inline constexpr const char* ADMIN = "admin";
} // namespace roles

/// Permission keys seeded by migration 0001, used by the authorization filter.
namespace permissions {
inline constexpr const char* LIBRARY_READ = "library.read";
inline constexpr const char* LIBRARY_MANAGE = "library.manage";
inline constexpr const char* GAME_READ = "game.read";
inline constexpr const char* GAME_DOWNLOAD = "game.download";
inline constexpr const char* GAME_PUBLISH = "game.publish";
inline constexpr const char* BUILD_UPLOAD = "build.upload";
inline constexpr const char* PATCHNOTE_WRITE = "patchnote.write";
inline constexpr const char* ADMIN_USERS_MANAGE = "admin.users.manage";
inline constexpr const char* ADMIN_ROLES_MANAGE = "admin.roles.manage";
inline constexpr const char* ADMIN_GAMES_MANAGE = "admin.games.manage";
inline constexpr const char* ADMIN_SETTINGS_MANAGE = "admin.settings.manage";
} // namespace permissions

struct Role {
    int id{0};
    std::string key;
    std::string description;
};

} // namespace launcher::domain
