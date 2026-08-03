#pragma once

#include <drogon/utils/coroutine.h>

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
};

} // namespace launcher::repositories
