#pragma once

#include <drogon/orm/DbClient.h>

#include "repositories/IAccountRepository.h"
#include "repositories/IRefreshTokenRepository.h"
#include "repositories/IRoleRepository.h"
#include "repositories/IUserRepository.h"
#include "repositories/IUserTokenRepository.h"

namespace launcher::repositories::postgres {

/// PostgreSQL implementations. This is the only place in the codebase where SQL is allowed
/// to appear, and every statement is parameterised — never concatenated.
///
/// uuid columns are compared against `$n::uuid` rather than the bare parameter: libpq sends
/// parameters as text, and PostgreSQL will not implicitly cast text to uuid in a comparison.

class PgUserRepository : public IUserRepository {
  public:
    explicit PgUserRepository(drogon::orm::DbClientPtr database);

    drogon::Task<std::optional<domain::User>> findById(std::string id) const override;

    drogon::Task<std::optional<domain::User>> findByEmail(std::string email) const override;

    drogon::Task<common::Result<domain::User>> create(domain::NewUser user) const override;

    drogon::Task<void> markEmailVerified(std::string userId) const override;

    drogon::Task<void> updatePasswordHash(std::string userId,
                                          std::string passwordHash) const override;

    drogon::Task<void> rehashPassword(std::string userId, std::string passwordHash) const override;

    drogon::Task<void> recordSuccessfulLogin(std::string userId) const override;

    drogon::Task<bool> chargeUpload(std::string userId, int64_t bytes) const override;

    drogon::Task<void> releaseUpload(std::string userId, int64_t bytes) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

class PgAccountRepository : public IAccountRepository {
  public:
    explicit PgAccountRepository(drogon::orm::DbClientPtr database);

    drogon::Task<bool>
    erase(std::string userId, ErasedIdentity identity, domain::NewAuditEntry audit) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

class PgRoleRepository : public IRoleRepository {
  public:
    explicit PgRoleRepository(drogon::orm::DbClientPtr database);

    drogon::Task<std::vector<std::string>> permissionsForUser(std::string userId) const override;

    drogon::Task<std::vector<std::string>> rolesForUser(std::string userId) const override;

    drogon::Task<bool> assignRole(std::string userId,
                                  std::string roleKey,
                                  std::optional<std::string> grantedBy) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

class PgRefreshTokenRepository : public IRefreshTokenRepository {
  public:
    explicit PgRefreshTokenRepository(drogon::orm::DbClientPtr database);

    drogon::Task<std::string> issue(NewRefreshToken token) const override;

    drogon::Task<std::optional<RefreshTokenRecord>>
    findByHash(std::string tokenHash) const override;

    drogon::Task<std::string> rotate(std::string previousId, NewRefreshToken next) const override;

    drogon::Task<void> revokeById(std::string id) const override;

    drogon::Task<void> revokeFamily(std::string familyId) const override;

    drogon::Task<void> revokeAllForUser(std::string userId) const override;

    drogon::Task<std::size_t> deleteExpired() const override;

  private:
    drogon::orm::DbClientPtr database_;
};

class PgUserTokenRepository : public IUserTokenRepository {
  public:
    explicit PgUserTokenRepository(drogon::orm::DbClientPtr database);

    drogon::Task<void> issue(std::string userId,
                             UserTokenPurpose purpose,
                             std::string tokenHash,
                             std::chrono::seconds ttl) const override;

    drogon::Task<std::optional<std::string>> consume(std::string tokenHash,
                                                     UserTokenPurpose purpose) const override;

    drogon::Task<void> invalidateAll(std::string userId, UserTokenPurpose purpose) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

} // namespace launcher::repositories::postgres
