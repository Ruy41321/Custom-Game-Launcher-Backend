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
#include "services/PasswordHasher.h"

namespace launcher::services {

/// Managing accounts, roles and upload quotas.
///
/// Every authorization rule lives here rather than in the controllers, so no route can be
/// added later that forgets one — the same discipline the catalog and upload services follow.
/// The audit entry describing each change is assembled here too and handed to the repository,
/// which writes it in the same statement as the change itself.
/// What an operator gets back after handing out a one-time password: the account as it now
/// stands, and the password itself, which exists nowhere else.
///
/// It is returned rather than delivered because this whole feature is for the deployment that
/// cannot deliver anything — there is no mail transport, which is why an operator is doing
/// this at all. It is therefore shown once, in the response to the request that created it,
/// and is never stored, logged or recoverable: only its Argon2id hash reaches the database,
/// and the audit entry records that it happened and not what it was.
struct TemporaryPassword {
    repositories::AdminUserSummary user;
    std::string password;
};

class AdminUserService {
  public:
    AdminUserService(const repositories::IAdminUserRepository& users,
                     const repositories::IAuditRepository& audit,
                     const IPasswordHasher& passwordHasher);

    drogon::Task<common::Result<repositories::AdminUserPage>>
    list(domain::Actor actor, repositories::AdminUserQuery query) const;

    drogon::Task<common::Result<repositories::AdminUserSummary>> find(domain::Actor actor,
                                                                      std::string userId) const;

    drogon::Task<common::Result<repositories::AdminUserSummary>>
    setUploadQuota(domain::Actor actor, std::string userId, int64_t quotaBytes) const;

    drogon::Task<common::Result<repositories::AdminUserSummary>>
    setActive(domain::Actor actor, std::string userId, bool active) const;

    /// Gives an account a password the operator can read out, and requires it to be changed.
    ///
    /// The way back in on a deployment that sends no mail: with `MAIL_TRANSPORT=none` there is
    /// no reset link, so "forgotten your password?" is a sentence telling the person to ask an
    /// operator, and this is what the operator does about it.
    ///
    /// **An operator cannot do this to themselves.** Same shape as the deactivation rule, and
    /// a sharper reason: the flag refuses every route but the password change, and the console
    /// this request arrives on has no such route — an operator who set their own would lock
    /// themselves out of the surface they administer, with only the public API left to escape
    /// through.
    drogon::Task<common::Result<TemporaryPassword>> setTemporaryPassword(domain::Actor actor,
                                                                         std::string userId) const;

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
    const IPasswordHasher& passwordHasher_;
};

} // namespace launcher::services
