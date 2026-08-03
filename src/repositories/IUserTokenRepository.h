#pragma once

#include <drogon/utils/coroutine.h>

#include <chrono>
#include <optional>
#include <string>

namespace launcher::repositories {

/// Email verification and password reset share one table: identical shape, identical
/// lifecycle (hashed, single use, expiring), distinguished only by purpose.
enum class UserTokenPurpose {
    EmailVerification,
    PasswordReset,
};

const char* toDatabaseValue(UserTokenPurpose purpose);

class IUserTokenRepository {
  public:
    virtual ~IUserTokenRepository() = default;

    virtual drogon::Task<void> issue(std::string userId,
                                     UserTokenPurpose purpose,
                                     std::string tokenHash,
                                     std::chrono::seconds ttl) const = 0;

    /// Atomically marks the token used and returns its owner, or nullopt when the token is
    /// unknown, already used or expired. One statement, so the same link cannot be redeemed
    /// twice by two concurrent requests.
    virtual drogon::Task<std::optional<std::string>> consume(std::string tokenHash,
                                                             UserTokenPurpose purpose) const = 0;

    /// Invalidates outstanding tokens of a purpose, e.g. after a successful password reset.
    virtual drogon::Task<void> invalidateAll(std::string userId,
                                             UserTokenPurpose purpose) const = 0;
};

} // namespace launcher::repositories
