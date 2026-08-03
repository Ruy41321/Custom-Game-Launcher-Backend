#pragma once

#include <drogon/utils/coroutine.h>

#include <optional>
#include <string>
#include <vector>

namespace launcher::repositories {

/// Reads the role/permission graph. Roles are rows, not an enum, so this layer never needs
/// to change when a new role is introduced.
class IRoleRepository {
  public:
    virtual ~IRoleRepository() = default;

    /// Flattened, de-duplicated permission keys across every role the user holds.
    virtual drogon::Task<std::vector<std::string>> permissionsForUser(std::string userId) const = 0;

    virtual drogon::Task<std::vector<std::string>> rolesForUser(std::string userId) const = 0;

    /// Idempotent: granting a role the user already has is not an error. Returns false when
    /// the role key does not exist.
    virtual drogon::Task<bool> assignRole(std::string userId,
                                          std::string roleKey,
                                          std::optional<std::string> grantedBy) const = 0;
};

} // namespace launcher::repositories
