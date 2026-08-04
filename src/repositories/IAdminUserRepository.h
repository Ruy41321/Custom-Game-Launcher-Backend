#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain/AuditEntry.h"
#include "domain/Role.h"
#include "domain/User.h"

namespace launcher::repositories {

inline constexpr int DEFAULT_ADMIN_USER_PAGE_SIZE = 25;
inline constexpr int MAX_ADMIN_USER_PAGE_SIZE = 100;

struct AdminUserQuery {
    /// Matched against the email address and the display name. Blank lists everybody.
    std::string search;
    /// Narrows to deactivated accounts, which is otherwise a needle in the whole table.
    bool onlyInactive{false};
    int limit{DEFAULT_ADMIN_USER_PAGE_SIZE};
    int offset{0};
};

/// An account as the operator console shows it: the domain user plus the things only this
/// surface cares about — which roles it holds and when it was last seen.
struct AdminUserSummary {
    domain::User user;
    std::vector<std::string> roles;
    std::string createdAt;
    /// Empty when the account has never signed in.
    std::string lastLoginAt;
};

struct AdminUserPage {
    std::vector<AdminUserSummary> items;
    int64_t total{0};
};

/// What happened to a role grant or revocation.
///
/// Spelled out rather than reported as a boolean because the three failures need different
/// answers: an unknown role is the caller's mistake, an unknown user is a 404, and a change
/// that was already in effect is a success with nothing to record.
enum class RoleChange {
    Applied,
    AlreadyInThatState,
    NoSuchUser,
    NoSuchRole,
};

/// Data access for the administrative surface.
///
/// Separate from IUserRepository because the operations differ in a way that matters: every
/// mutation here takes the audit entry that describes it and writes it in the *same*
/// statement. That is what makes the trail obligatory rather than best-effort — no ordering,
/// no second round trip, and no way for a change to land without its record. A row is written
/// only when the change actually took effect, which the CTEs below express by selecting from
/// the modifying arm.
///
/// Every parameter is taken by value: a coroutine's reference parameters dangle as soon as it
/// first suspends.
class IAdminUserRepository {
  public:
    virtual ~IAdminUserRepository() = default;

    virtual drogon::Task<AdminUserPage> search(AdminUserQuery query) const = 0;

    virtual drogon::Task<std::optional<AdminUserSummary>> findById(std::string userId) const = 0;

    virtual drogon::Task<std::optional<AdminUserSummary>>
    setUploadQuota(std::string userId, int64_t quotaBytes, domain::NewAuditEntry audit) const = 0;

    virtual drogon::Task<std::optional<AdminUserSummary>>
    setActive(std::string userId, bool active, domain::NewAuditEntry audit) const = 0;

    virtual drogon::Task<RoleChange>
    grantRole(std::string userId, std::string roleKey, domain::NewAuditEntry audit) const = 0;

    virtual drogon::Task<RoleChange>
    revokeRole(std::string userId, std::string roleKey, domain::NewAuditEntry audit) const = 0;

    virtual drogon::Task<std::vector<domain::Role>> listRoles() const = 0;

    /// How many *other* active accounts still reach the given permission through some role.
    ///
    /// Exists for one rule: an operator must not be able to remove the last administrator,
    /// which would leave the surface unreachable with no endpoint left to repair it.
    virtual drogon::Task<int64_t> countOtherHoldersOf(std::string permissionKey,
                                                      std::string excludingUserId) const = 0;
};

} // namespace launcher::repositories
