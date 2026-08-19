#pragma once

#include <drogon/orm/DbClient.h>

#include "repositories/IAdminUserRepository.h"
#include "repositories/IAnalyticsRepository.h"
#include "repositories/IAuditRepository.h"

namespace launcher::repositories::postgres {

/// PostgreSQL implementations for the administrative surface.
///
/// Every mutation here is a single statement that changes a row *and* appends the audit entry
/// describing it, so the two cannot come apart. The audit arm selects from the modifying arm,
/// which means a change that did not take effect — an unknown account, a role already held —
/// records nothing.

class PgAdminUserRepository : public IAdminUserRepository {
  public:
    explicit PgAdminUserRepository(drogon::orm::DbClientPtr database);

    drogon::Task<AdminUserPage> search(AdminUserQuery query) const override;

    drogon::Task<std::optional<AdminUserSummary>> findById(std::string userId) const override;

    drogon::Task<std::optional<AdminUserSummary>> setUploadQuota(
        std::string userId, int64_t quotaBytes, domain::NewAuditEntry audit) const override;

    drogon::Task<std::optional<AdminUserSummary>>
    setActive(std::string userId, bool active, domain::NewAuditEntry audit) const override;

    drogon::Task<std::optional<AdminUserSummary>> setTemporaryPassword(
        std::string userId, std::string passwordHash, domain::NewAuditEntry audit) const override;

    drogon::Task<RoleChange>
    grantRole(std::string userId, std::string roleKey, domain::NewAuditEntry audit) const override;

    drogon::Task<RoleChange>
    revokeRole(std::string userId, std::string roleKey, domain::NewAuditEntry audit) const override;

    drogon::Task<std::vector<domain::Role>> listRoles() const override;

    drogon::Task<int64_t> countOtherHoldersOf(std::string permissionKey,
                                              std::string excludingUserId) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

class PgAnalyticsRepository : public IAnalyticsRepository {
  public:
    explicit PgAnalyticsRepository(drogon::orm::DbClientPtr database);

    drogon::Task<DownloadReport> downloadReport(int days, int topGames) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

class PgAuditRepository : public IAuditRepository {
  public:
    explicit PgAuditRepository(drogon::orm::DbClientPtr database);

    drogon::Task<AuditPage> search(AuditQuery query) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

} // namespace launcher::repositories::postgres
