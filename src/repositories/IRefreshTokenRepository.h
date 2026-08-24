#pragma once

#include <drogon/utils/coroutine.h>

#include <chrono>
#include <optional>
#include <string>

namespace launcher::repositories {

/// A stored refresh token, as far as the auth logic needs to know.
///
/// The token value itself is never stored or returned — only its hash is persisted, so a
/// database leak does not hand out live sessions.
struct RefreshTokenRecord {
    std::string id;
    std::string userId;
    std::string familyId;
    bool revoked{false};
    bool expired{false};
};

struct NewRefreshToken {
    std::string userId;
    std::string familyId;
    std::string tokenHash;
    std::chrono::seconds ttl{0};
    std::string userAgent;
    std::string ipAddress;
};

class IRefreshTokenRepository {
  public:
    virtual ~IRefreshTokenRepository() = default;

    virtual drogon::Task<std::string> issue(NewRefreshToken token) const = 0;

    virtual drogon::Task<std::optional<RefreshTokenRecord>>
    findByHash(std::string tokenHash) const = 0;

    /// Revokes `previousId` and issues its replacement in one transaction, recording the
    /// link between them. Rotation must be atomic: a crash in between would either leave
    /// the user with no valid token or leave two valid tokens in the same family.
    virtual drogon::Task<std::string> rotate(std::string previousId,
                                             NewRefreshToken next) const = 0;

    virtual drogon::Task<void> revokeById(std::string id) const = 0;

    /// Used on reuse detection: presenting an already-rotated token means the token leaked,
    /// so every descendant of the same original login is burned.
    virtual drogon::Task<void> revokeFamily(std::string familyId) const = 0;

    virtual drogon::Task<void> revokeAllForUser(std::string userId) const = 0;

    /// Housekeeping for expired rows; returns how many were removed.
    virtual drogon::Task<std::size_t> deleteExpired() const = 0;
};

} // namespace launcher::repositories
