#include "services/AdminUserService.h"

#include <algorithm>
#include <utility>

#include "domain/AuditEntry.h"
#include "domain/Validation.h"

namespace launcher::services {
namespace {

using common::ErrorCode;
using common::Result;
using common::VoidResult;
using repositories::AdminUserSummary;
using repositories::RoleChange;

namespace permissions = launcher::domain::permissions;
namespace actions = launcher::domain::auditActions;
namespace entities = launcher::domain::auditEntities;

/// A malformed identifier is an account that does not exist, not a server error.
///
/// Checked before the value can reach a `$n::uuid` comparison: PostgreSQL raises on a
/// malformed uuid literal, so without this a mistyped id in the console's address bar is a
/// 500 rather than the 404 it plainly is.
VoidResult requireUserId(const std::string& userId) {
    if (!domain::isUuid(userId)) {
        return VoidResult::failure(ErrorCode::NotFound, "no such account");
    }
    return VoidResult::success();
}

VoidResult requirePermission(const domain::Actor& actor, const char* permission) {
    if (!actor.can(permission)) {
        return VoidResult::failure(ErrorCode::Forbidden, "you do not have permission to do that");
    }
    return VoidResult::success();
}

domain::NewAuditEntry auditFor(const domain::Actor& actor,
                               const char* action,
                               std::string userId,
                               std::vector<domain::AuditField> metadata) {
    domain::NewAuditEntry entry;
    entry.actorUserId = actor.userId;
    entry.action = action;
    entry.entityType = entities::USER;
    entry.entityId = std::move(userId);
    entry.metadata = std::move(metadata);
    return entry;
}

Result<AdminUserSummary> roleChangeToResult(RoleChange change, const std::string& roleKey) {
    switch (change) {
    case RoleChange::NoSuchUser:
        return Result<AdminUserSummary>::failure(ErrorCode::NotFound, "no such account");
    case RoleChange::NoSuchRole:
        return Result<AdminUserSummary>::failure(ErrorCode::InvalidInput,
                                                 "no such role: " + roleKey);
    case RoleChange::Applied:
    case RoleChange::AlreadyInThatState:
        break;
    }
    // Both remaining cases are a success: granting a role the account already holds is the
    // same end state, and an endpoint that failed on it would make retrying a lost response
    // dangerous for no reason.
    return Result<AdminUserSummary>::success(AdminUserSummary{});
}

} // namespace

AdminUserService::AdminUserService(const repositories::IAdminUserRepository& users,
                                   const repositories::IAuditRepository& audit)
    : users_(users),
      audit_(audit) {}

drogon::Task<Result<repositories::AdminUserPage>>
AdminUserService::list(domain::Actor actor, repositories::AdminUserQuery query) const {
    if (auto allowed = requirePermission(actor, permissions::ADMIN_USERS_MANAGE); !allowed.ok()) {
        co_return Result<repositories::AdminUserPage>::failure(allowed.error());
    }

    query.limit = std::clamp(query.limit, 1, repositories::MAX_ADMIN_USER_PAGE_SIZE);
    query.offset = std::max(query.offset, 0);

    co_return Result<repositories::AdminUserPage>::success(
        co_await users_.search(std::move(query)));
}

drogon::Task<Result<AdminUserSummary>> AdminUserService::find(domain::Actor actor,
                                                              std::string userId) const {
    if (auto allowed = requirePermission(actor, permissions::ADMIN_USERS_MANAGE); !allowed.ok()) {
        co_return Result<AdminUserSummary>::failure(allowed.error());
    }

    if (auto valid = requireUserId(userId); !valid.ok()) {
        co_return Result<AdminUserSummary>::failure(valid.error());
    }

    auto found = co_await users_.findById(std::move(userId));
    if (!found.has_value()) {
        co_return Result<AdminUserSummary>::failure(ErrorCode::NotFound, "no such account");
    }
    co_return Result<AdminUserSummary>::success(std::move(*found));
}

drogon::Task<Result<AdminUserSummary>> AdminUserService::setUploadQuota(domain::Actor actor,
                                                                        std::string userId,
                                                                        int64_t quotaBytes) const {
    if (auto allowed = requirePermission(actor, permissions::ADMIN_USERS_MANAGE); !allowed.ok()) {
        co_return Result<AdminUserSummary>::failure(allowed.error());
    }
    if (auto valid = requireUserId(userId); !valid.ok()) {
        co_return Result<AdminUserSummary>::failure(valid.error());
    }
    if (quotaBytes < 0) {
        co_return Result<AdminUserSummary>::failure(ErrorCode::InvalidInput,
                                                    "quotaBytes cannot be negative");
    }

    // Lowering a quota below what the account has already stored is allowed on purpose: it
    // stops further uploads without deleting anything, which is the tool an operator reaches
    // for when somebody is filling the disk. The charge is a conditional statement, so an
    // account over its quota simply cannot upload again.
    // Built before the suspension rather than inside the co_await expression: a braced
    // initialiser as a coroutine argument is what GCC 13 crashes on, and a named local is what
    // the by-value rule for coroutine parameters wants anyway.
    std::vector<domain::AuditField> fields{{"quotaBytes", std::to_string(quotaBytes)}};
    auto entry = auditFor(actor, actions::USER_QUOTA_CHANGED, userId, std::move(fields));

    auto updated = co_await users_.setUploadQuota(userId, quotaBytes, std::move(entry));

    if (!updated.has_value()) {
        co_return Result<AdminUserSummary>::failure(ErrorCode::NotFound, "no such account");
    }
    co_return Result<AdminUserSummary>::success(std::move(*updated));
}

drogon::Task<Result<AdminUserSummary>>
AdminUserService::setActive(domain::Actor actor, std::string userId, bool active) const {
    if (auto allowed = requirePermission(actor, permissions::ADMIN_USERS_MANAGE); !allowed.ok()) {
        co_return Result<AdminUserSummary>::failure(allowed.error());
    }

    if (auto valid = requireUserId(userId); !valid.ok()) {
        co_return Result<AdminUserSummary>::failure(valid.error());
    }

    if (!active) {
        if (actor.owns(userId)) {
            co_return Result<AdminUserSummary>::failure(
                ErrorCode::InvalidInput,
                "an operator cannot deactivate their own account; ask another one");
        }
        if (auto safe = co_await refuseIfLastAdministrator(userId); !safe.ok()) {
            co_return Result<AdminUserSummary>::failure(safe.error());
        }
    }

    auto entry =
        auditFor(actor, active ? actions::USER_ACTIVATED : actions::USER_DEACTIVATED, userId, {});

    auto updated = co_await users_.setActive(userId, active, std::move(entry));

    if (!updated.has_value()) {
        co_return Result<AdminUserSummary>::failure(ErrorCode::NotFound, "no such account");
    }
    co_return Result<AdminUserSummary>::success(std::move(*updated));
}

drogon::Task<Result<AdminUserSummary>>
AdminUserService::grantRole(domain::Actor actor, std::string userId, std::string roleKey) const {
    if (auto allowed = requirePermission(actor, permissions::ADMIN_ROLES_MANAGE); !allowed.ok()) {
        co_return Result<AdminUserSummary>::failure(allowed.error());
    }

    if (auto valid = requireUserId(userId); !valid.ok()) {
        co_return Result<AdminUserSummary>::failure(valid.error());
    }

    std::vector<domain::AuditField> fields{{"role", roleKey}};
    auto entry = auditFor(actor, actions::ROLE_GRANTED, userId, std::move(fields));

    const auto change = co_await users_.grantRole(userId, roleKey, std::move(entry));

    if (auto classified = roleChangeToResult(change, roleKey); !classified.ok()) {
        co_return classified;
    }

    // Read back rather than returning what the change reported: the caller wants the account
    // as it now stands, roles included, and only a fresh read has that.
    auto found = co_await users_.findById(std::move(userId));
    co_return found.has_value()
        ? Result<AdminUserSummary>::success(std::move(*found))
        : Result<AdminUserSummary>::failure(ErrorCode::NotFound, "no such account");
}

drogon::Task<Result<AdminUserSummary>>
AdminUserService::revokeRole(domain::Actor actor, std::string userId, std::string roleKey) const {
    if (auto allowed = requirePermission(actor, permissions::ADMIN_ROLES_MANAGE); !allowed.ok()) {
        co_return Result<AdminUserSummary>::failure(allowed.error());
    }
    if (auto valid = requireUserId(userId); !valid.ok()) {
        co_return Result<AdminUserSummary>::failure(valid.error());
    }
    if (auto safe = co_await refuseIfLastAdministrator(userId); !safe.ok()) {
        co_return Result<AdminUserSummary>::failure(safe.error());
    }

    std::vector<domain::AuditField> fields{{"role", roleKey}};
    auto entry = auditFor(actor, actions::ROLE_REVOKED, userId, std::move(fields));

    const auto change = co_await users_.revokeRole(userId, roleKey, std::move(entry));

    if (auto classified = roleChangeToResult(change, roleKey); !classified.ok()) {
        co_return classified;
    }

    auto found = co_await users_.findById(std::move(userId));
    co_return found.has_value()
        ? Result<AdminUserSummary>::success(std::move(*found))
        : Result<AdminUserSummary>::failure(ErrorCode::NotFound, "no such account");
}

drogon::Task<Result<std::vector<domain::Role>>>
AdminUserService::listRoles(domain::Actor actor) const {
    if (auto allowed = requirePermission(actor, permissions::ADMIN_ROLES_MANAGE); !allowed.ok()) {
        co_return Result<std::vector<domain::Role>>::failure(allowed.error());
    }
    co_return Result<std::vector<domain::Role>>::success(co_await users_.listRoles());
}

drogon::Task<Result<repositories::AuditPage>>
AdminUserService::listAudit(domain::Actor actor, repositories::AuditQuery query) const {
    // The broadest administrative permission, because the trail spans every subsystem: reading
    // who did what across all of them is an operator-wide capability, not a per-subsystem one.
    if (auto allowed = requirePermission(actor, permissions::ADMIN_SETTINGS_MANAGE);
        !allowed.ok()) {
        co_return Result<repositories::AuditPage>::failure(allowed.error());
    }

    query.limit = std::clamp(query.limit, 1, repositories::MAX_AUDIT_PAGE_SIZE);
    query.offset = std::max(query.offset, 0);

    co_return Result<repositories::AuditPage>::success(co_await audit_.search(std::move(query)));
}

drogon::Task<VoidResult> AdminUserService::refuseIfLastAdministrator(std::string userId) const {
    const auto others =
        co_await users_.countOtherHoldersOf(permissions::ADMIN_USERS_MANAGE, std::move(userId));
    if (others > 0) {
        co_return VoidResult::success();
    }
    co_return VoidResult::failure(
        ErrorCode::Conflict,
        "this is the only active account that can manage users; grant another one first");
}

} // namespace launcher::services
