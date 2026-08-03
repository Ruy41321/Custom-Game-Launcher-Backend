#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <string>

#include "common/Result.h"
#include "domain/User.h"

namespace launcher::repositories {

/// Persistence for accounts.
///
/// Every parameter is taken **by value**. A coroutine's reference parameters dangle the
/// moment it first suspends, because the caller's frame may already be gone; passing by
/// value moves the argument into the coroutine frame. This is a correctness rule across the
/// whole repository layer, not a style choice.
class IUserRepository {
  public:
    virtual ~IUserRepository() = default;

    virtual drogon::Task<std::optional<domain::User>> findById(std::string id) const = 0;

    virtual drogon::Task<std::optional<domain::User>> findByEmail(std::string email) const = 0;

    /// Conflict when the email is already registered. Detected from the unique constraint
    /// rather than a preceding SELECT, so two simultaneous registrations cannot both pass.
    virtual drogon::Task<common::Result<domain::User>> create(domain::NewUser user) const = 0;

    virtual drogon::Task<void> markEmailVerified(std::string userId) const = 0;

    virtual drogon::Task<void> updatePasswordHash(std::string userId,
                                                  std::string passwordHash) const = 0;

    virtual drogon::Task<void> recordSuccessfulLogin(std::string userId) const = 0;

    /// Books `bytes` against the cumulative upload allowance, refusing when that would take
    /// the account past its quota.
    ///
    /// The check and the increment are one conditional statement on purpose: a read followed
    /// by a write lets concurrent uploads each see room that only one of them can have.
    /// False means the quota is exhausted, and nothing was charged.
    virtual drogon::Task<bool> chargeUpload(std::string userId, int64_t bytes) const = 0;

    /// Gives bytes back when a charged upload turns out not to have stored anything new.
    /// Clamped at zero so a double release can never make the counter negative.
    virtual drogon::Task<void> releaseUpload(std::string userId, int64_t bytes) const = 0;
};

} // namespace launcher::repositories
