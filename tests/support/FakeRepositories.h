#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "common/Random.h"
#include "repositories/IAccountRepository.h"
#include "repositories/IRefreshTokenRepository.h"
#include "repositories/IRoleRepository.h"
#include "repositories/IUserRepository.h"
#include "repositories/IUserTokenRepository.h"

namespace launcher::testing {

/// In-memory repository doubles for service unit tests.
///
/// Hand-written rather than gmock: the interfaces return coroutines, which gmock can only
/// express awkwardly, and these fakes have to model real behaviour anyway (unique emails,
/// single-use tokens, expiry) for the tests to mean anything.
///
/// The interface methods are const because callers only ever read through them, so the
/// recorded state is mutable.

class FakeUserRepository : public repositories::IUserRepository {
  public:
    mutable std::vector<domain::User> users;
    mutable std::vector<std::string> verifiedUserIds;
    mutable std::vector<std::string> loginRecordedFor;
    mutable std::map<std::string, std::string> passwordUpdates;
    mutable std::map<std::string, std::string> rehashes;

    domain::User seed(domain::User user) const {
        if (user.id.empty()) {
            user.id = common::randomUuid();
        }
        users.push_back(user);
        return user;
    }

    drogon::Task<std::optional<domain::User>> findById(std::string id) const override {
        for (const auto& user : users) {
            if (user.id == id) {
                co_return user;
            }
        }
        co_return std::nullopt;
    }

    drogon::Task<std::optional<domain::User>> findByEmail(std::string email) const override {
        for (const auto& user : users) {
            if (user.email == email) {
                co_return user;
            }
        }
        co_return std::nullopt;
    }

    drogon::Task<common::Result<domain::User>> create(domain::NewUser candidate) const override {
        for (const auto& user : users) {
            if (user.email == candidate.email) {
                co_return common::Result<domain::User>::failure(
                    common::ErrorCode::Conflict, "that email address is already registered");
            }
        }

        domain::User created;
        created.id = common::randomUuid();
        created.email = candidate.email;
        created.displayName = candidate.displayName;
        created.passwordHash = candidate.passwordHash;
        created.uploadQuotaBytes = 5LL * 1024 * 1024 * 1024;
        users.push_back(created);
        co_return common::Result<domain::User>::success(created);
    }

    drogon::Task<void> markEmailVerified(std::string userId) const override {
        verifiedUserIds.push_back(userId);
        for (auto& user : users) {
            if (user.id == userId) {
                user.emailVerified = true;
            }
        }
        co_return;
    }

    drogon::Task<void> updatePasswordHash(std::string userId,
                                          std::string passwordHash) const override {
        passwordUpdates[userId] = passwordHash;
        for (auto& user : users) {
            if (user.id == userId) {
                user.passwordHash = passwordHash;
                user.passwordChangeRequired = false;
            }
        }
        co_return;
    }

    drogon::Task<void> rehashPassword(std::string userId, std::string passwordHash) const override {
        rehashes[userId] = passwordHash;
        for (auto& user : users) {
            if (user.id == userId) {
                user.passwordHash = passwordHash;
            }
        }
        co_return;
    }

    drogon::Task<void> recordSuccessfulLogin(std::string userId) const override {
        loginRecordedFor.push_back(userId);
        co_return;
    }

    drogon::Task<bool> chargeUpload(std::string userId, int64_t bytes) const override {
        for (auto& user : users) {
            if (user.id != userId) {
                continue;
            }
            if (user.uploadUsedBytes + bytes > user.uploadQuotaBytes) {
                co_return false;
            }
            user.uploadUsedBytes += bytes;
            co_return true;
        }
        co_return false;
    }

    drogon::Task<void> releaseUpload(std::string userId, int64_t bytes) const override {
        for (auto& user : users) {
            if (user.id == userId) {
                user.uploadUsedBytes = std::max<int64_t>(0, user.uploadUsedBytes - bytes);
            }
        }
        co_return;
    }
};

/// Stands in for the one erasing statement.
///
/// Deliberately does the anonymising *and* the audit entry together, in that order and with no
/// way to do one without the other, because that is the property the production statement
/// exists to guarantee — a fake that let them come apart would let a service bug through.
class FakeAccountRepository : public repositories::IAccountRepository {
  public:
    /// The users the erasure acts on. Point this at the same fake the service reads through.
    FakeUserRepository* users{nullptr};

    mutable std::vector<domain::NewAuditEntry> recorded;
    mutable std::vector<std::string> reasons;
    /// Set to make the statement fail, so a test can assert that a failed erasure records
    /// nothing — the half the audit rule is really about.
    bool refuse{false};

    drogon::Task<bool> erase(std::string userId,
                             repositories::ErasedIdentity identity,
                             domain::NewAuditEntry audit) const override {
        if (refuse || users == nullptr) {
            co_return false;
        }

        for (auto& user : users->users) {
            if (user.id != userId || user.email == identity.email) {
                continue;
            }
            user.email = identity.email;
            user.displayName = identity.displayName;
            user.passwordHash = identity.passwordHash;
            user.emailVerified = false;
            user.isActive = false;

            const_cast<std::vector<std::string>&>(reasons).push_back(identity.reason);
            const_cast<std::vector<domain::NewAuditEntry>&>(recorded).push_back(std::move(audit));
            co_return true;
        }
        co_return false;
    }
};

class FakeRoleRepository : public repositories::IRoleRepository {
  public:
    mutable std::map<std::string, std::vector<std::string>> rolesByUser;
    mutable std::map<std::string, std::vector<std::string>> permissionsByUser;
    mutable std::vector<std::pair<std::string, std::string>> assignments;

    drogon::Task<std::vector<std::string>> permissionsForUser(std::string userId) const override {
        const auto found = permissionsByUser.find(userId);
        co_return found == permissionsByUser.end() ? std::vector<std::string>{} : found->second;
    }

    drogon::Task<std::vector<std::string>> rolesForUser(std::string userId) const override {
        const auto found = rolesByUser.find(userId);
        co_return found == rolesByUser.end() ? std::vector<std::string>{} : found->second;
    }

    drogon::Task<bool>
    assignRole(std::string userId, std::string roleKey, std::optional<std::string>) const override {
        assignments.emplace_back(userId, roleKey);
        auto& roles = rolesByUser[userId];
        if (std::find(roles.begin(), roles.end(), roleKey) == roles.end()) {
            roles.push_back(roleKey);
        }
        co_return true;
    }
};

class FakeRefreshTokenRepository : public repositories::IRefreshTokenRepository {
  public:
    struct Entry {
        repositories::RefreshTokenRecord record;
        std::string tokenHash;
    };

    mutable std::vector<Entry> entries;
    mutable std::vector<std::string> revokedFamilies;
    mutable std::vector<std::string> revokedIds;
    mutable std::vector<std::string> revokedUsers;
    mutable std::vector<std::pair<std::string, std::string>> rotations; ///< (previousId, newId)

    const Entry* findEntryByHash(const std::string& hash) const {
        for (const auto& entry : entries) {
            if (entry.tokenHash == hash) {
                return &entry;
            }
        }
        return nullptr;
    }

    drogon::Task<std::string> issue(repositories::NewRefreshToken token) const override {
        Entry entry;
        entry.record.id = common::randomUuid();
        entry.record.userId = token.userId;
        entry.record.familyId = token.familyId;
        entry.tokenHash = token.tokenHash;
        entries.push_back(entry);
        co_return entry.record.id;
    }

    drogon::Task<std::optional<repositories::RefreshTokenRecord>>
    findByHash(std::string tokenHash) const override {
        for (const auto& entry : entries) {
            if (entry.tokenHash == tokenHash) {
                co_return entry.record;
            }
        }
        co_return std::nullopt;
    }

    drogon::Task<std::string> rotate(std::string previousId,
                                     repositories::NewRefreshToken next) const override {
        for (auto& entry : entries) {
            if (entry.record.id == previousId) {
                entry.record.revoked = true;
            }
        }

        Entry entry;
        entry.record.id = common::randomUuid();
        entry.record.userId = next.userId;
        entry.record.familyId = next.familyId;
        entry.tokenHash = next.tokenHash;
        entries.push_back(entry);

        rotations.emplace_back(previousId, entry.record.id);
        co_return entry.record.id;
    }

    drogon::Task<void> revokeById(std::string id) const override {
        revokedIds.push_back(id);
        for (auto& entry : entries) {
            if (entry.record.id == id) {
                entry.record.revoked = true;
            }
        }
        co_return;
    }

    drogon::Task<void> revokeFamily(std::string familyId) const override {
        revokedFamilies.push_back(familyId);
        for (auto& entry : entries) {
            if (entry.record.familyId == familyId) {
                entry.record.revoked = true;
            }
        }
        co_return;
    }

    drogon::Task<void> revokeAllForUser(std::string userId) const override {
        revokedUsers.push_back(userId);
        for (auto& entry : entries) {
            if (entry.record.userId == userId) {
                entry.record.revoked = true;
            }
        }
        co_return;
    }

    drogon::Task<std::size_t> deleteExpired() const override { co_return 0U; }
};

class FakeUserTokenRepository : public repositories::IUserTokenRepository {
  public:
    struct Entry {
        std::string userId;
        repositories::UserTokenPurpose purpose;
        std::string tokenHash;
        bool consumed{false};
        bool expired{false};
    };

    mutable std::vector<Entry> entries;
    mutable std::vector<std::pair<std::string, repositories::UserTokenPurpose>> invalidations;

    drogon::Task<void> issue(std::string userId,
                             repositories::UserTokenPurpose purpose,
                             std::string tokenHash,
                             std::chrono::seconds) const override {
        entries.push_back(Entry{userId, purpose, tokenHash, false, false});
        co_return;
    }

    drogon::Task<std::optional<std::string>>
    consume(std::string tokenHash, repositories::UserTokenPurpose purpose) const override {
        for (auto& entry : entries) {
            if (entry.tokenHash == tokenHash && entry.purpose == purpose && !entry.consumed &&
                !entry.expired) {
                entry.consumed = true;
                co_return entry.userId;
            }
        }
        co_return std::nullopt;
    }

    drogon::Task<void> invalidateAll(std::string userId,
                                     repositories::UserTokenPurpose purpose) const override {
        invalidations.emplace_back(userId, purpose);
        for (auto& entry : entries) {
            if (entry.userId == userId && entry.purpose == purpose) {
                entry.consumed = true;
            }
        }
        co_return;
    }
};

} // namespace launcher::testing
