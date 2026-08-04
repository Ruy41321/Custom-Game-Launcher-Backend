#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/Result.h"
#include "domain/Actor.h"
#include "domain/Role.h"
#include "repositories/IAdminUserRepository.h"
#include "repositories/IAuditRepository.h"

namespace launcher::services {

/// Managing accounts, roles and upload quotas.
///
/// Every authorization rule lives here rather than in the controllers, so no route can be
/// added later that forgets one — the same discipline the catalog and upload services follow.
/// The audit entry describing each change is assembled here too and handed to the repository,
/// which writes it in the same statement as the change itself.
class AdminUserService {
  public:
    AdminUserService(const repositories::IAdminUserRepository& users,
                     const repositories::IAuditRepository& audit);

    drogon::Task<common::Result<repositories::AdminUserPage>>
    list(domain::Actor actor, repositories::AdminUserQuery query) const;

    drogon::Task<common::Result<repositories::AdminUserSummary>> find(domain::Actor actor,
                                                                      std::string userId) const;

    drogon::Task<common::Result<repositories::AdminUserSummary>>
    setUploadQuota(domain::Actor actor, std::string userId, int64_t quotaBytes) const;

    drogon::Task<common::Result<repositories::AdminUserSummary>>
    setActive(domain::Actor actor, std::string userId, bool active) const;

    drogon::Task<common::Result<repositories::AdminUserSummary>>
    grantRole(domain::Actor actor, std::string userId, std::string roleKey) const;

    drogon::Task<common::Result<repositories::AdminUserSummary>>
    revokeRole(domain::Actor actor, std::string userId, std::string roleKey) const;

    drogon::Task<common::Result<std::vector<domain::Role>>> listRoles(domain::Actor actor) const;

    drogon::Task<common::Result<repositories::AuditPage>>
    listAudit(domain::Actor actor, repositories::AuditQuery query) const;

  private:
    /// Refuses a change that would leave the deployment with no way back in.
    ///
    /// The rule is deliberately blunt: while an account is the only active holder of
    /// `admin.users.manage`, neither its roles nor its active flag may be changed by anybody,
    /// including itself. A finer rule would have to reason about which permissions the
    /// particular role being revoked carries, and being wrong about that once costs the
    /// operator every route back — there is no endpoint that repairs an empty admin list, only
    /// the command line.
    drogon::Task<common::VoidResult> refuseIfLastAdministrator(std::string userId) const;

    const repositories::IAdminUserRepository& users_;
    const repositories::IAuditRepository& audit_;
};

} // namespace launcher::services
