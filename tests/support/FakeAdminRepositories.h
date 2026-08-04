#pragma once

#include <drogon/utils/coroutine.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "repositories/IAdminUserRepository.h"
#include "repositories/IAuditRepository.h"

namespace launcher::testing {

/// In-memory stand-ins for the administrative repositories.
///
/// The audit entries land in the same object as the change that produced them, mirroring the
/// production arrangement where both are one statement: a test can therefore assert that a
/// refused change recorded nothing, which is the property that matters.
class FakeAdminUserRepository : public repositories::IAdminUserRepository {
  public:
    struct Account {
        domain::User user;
        std::vector<std::string> roles;
    };

    std::map<std::string, Account> accounts;
    std::map<std::string, std::vector<std::string>> permissionsByRole;
    std::vector<domain::NewAuditEntry> recorded;

    domain::User& seed(const std::string& id, const std::string& email) {
        auto& account = accounts[id];
        account.user.id = id;
        account.user.email = email;
        account.user.displayName = email;
        account.user.isActive = true;
        account.user.uploadQuotaBytes = 1024;
        return account.user;
    }

    void grant(const std::string& id, const std::string& roleKey) {
        accounts[id].roles.push_back(roleKey);
    }

    drogon::Task<repositories::AdminUserPage>
    search(repositories::AdminUserQuery query) const override {
        repositories::AdminUserPage page;
        for (const auto& [id, account] : accounts) {
            if (query.onlyInactive && account.user.isActive) {
                continue;
            }
            if (!query.search.empty() &&
                account.user.email.find(query.search) == std::string::npos) {
                continue;
            }
            page.items.push_back(summarise(account));
        }
        page.total = static_cast<int64_t>(page.items.size());
        co_return page;
    }

    drogon::Task<std::optional<repositories::AdminUserSummary>>
    findById(std::string userId) const override {
        const auto found = accounts.find(userId);
        if (found == accounts.end()) {
            co_return std::nullopt;
        }
        co_return summarise(found->second);
    }

    drogon::Task<std::optional<repositories::AdminUserSummary>> setUploadQuota(
        std::string userId, int64_t quotaBytes, domain::NewAuditEntry audit) const override {
        const auto found = accounts.find(userId);
        if (found == accounts.end()) {
            co_return std::nullopt;
        }
        mutable_(found->second).user.uploadQuotaBytes = quotaBytes;
        record(std::move(audit));
        co_return summarise(found->second);
    }

    drogon::Task<std::optional<repositories::AdminUserSummary>>
    setActive(std::string userId, bool active, domain::NewAuditEntry audit) const override {
        const auto found = accounts.find(userId);
        if (found == accounts.end()) {
            co_return std::nullopt;
        }
        mutable_(found->second).user.isActive = active;
        record(std::move(audit));
        co_return summarise(found->second);
    }

    drogon::Task<repositories::RoleChange>
    grantRole(std::string userId, std::string roleKey, domain::NewAuditEntry audit) const override {
        const auto found = accounts.find(userId);
        if (found == accounts.end()) {
            co_return repositories::RoleChange::NoSuchUser;
        }
        if (!permissionsByRole.count(roleKey)) {
            co_return repositories::RoleChange::NoSuchRole;
        }

        auto& roles = mutable_(found->second).roles;
        if (std::find(roles.begin(), roles.end(), roleKey) != roles.end()) {
            co_return repositories::RoleChange::AlreadyInThatState;
        }
        roles.push_back(roleKey);
        record(std::move(audit));
        co_return repositories::RoleChange::Applied;
    }

    drogon::Task<repositories::RoleChange> revokeRole(std::string userId,
                                                      std::string roleKey,
                                                      domain::NewAuditEntry audit) const override {
        const auto found = accounts.find(userId);
        if (found == accounts.end()) {
            co_return repositories::RoleChange::NoSuchUser;
        }
        if (!permissionsByRole.count(roleKey)) {
            co_return repositories::RoleChange::NoSuchRole;
        }

        auto& roles = mutable_(found->second).roles;
        const auto at = std::find(roles.begin(), roles.end(), roleKey);
        if (at == roles.end()) {
            co_return repositories::RoleChange::AlreadyInThatState;
        }
        roles.erase(at);
        record(std::move(audit));
        co_return repositories::RoleChange::Applied;
    }

    drogon::Task<std::vector<domain::Role>> listRoles() const override {
        std::vector<domain::Role> roles;
        for (const auto& [key, unused] : permissionsByRole) {
            static_cast<void>(unused);
            domain::Role role;
            role.key = key;
            roles.push_back(std::move(role));
        }
        co_return roles;
    }

    drogon::Task<int64_t> countOtherHoldersOf(std::string permissionKey,
                                              std::string excludingUserId) const override {
        int64_t holders = 0;
        for (const auto& [id, account] : accounts) {
            if (id == excludingUserId || !account.user.isActive) {
                continue;
            }
            for (const auto& roleKey : account.roles) {
                const auto granted = permissionsByRole.find(roleKey);
                if (granted != permissionsByRole.end() &&
                    std::find(granted->second.begin(), granted->second.end(), permissionKey) !=
                        granted->second.end()) {
                    ++holders;
                    break;
                }
            }
        }
        co_return holders;
    }

  private:
    /// The interface is const throughout, because a repository is injected as a const
    /// reference everywhere. A fake still has to remember what happened to it.
    Account& mutable_(const Account& account) const {
        return const_cast<Account&>(account); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }

    void record(domain::NewAuditEntry audit) const {
        const_cast<std::vector<domain::NewAuditEntry>&>(recorded).push_back(std::move(audit));
    }

    static repositories::AdminUserSummary summarise(const Account& account) {
        repositories::AdminUserSummary summary;
        summary.user = account.user;
        summary.roles = account.roles;
        summary.createdAt = "2026-01-01T00:00:00Z";
        return summary;
    }
};

class FakeAuditRepository : public repositories::IAuditRepository {
  public:
    std::vector<domain::AuditEntry> entries;

    drogon::Task<repositories::AuditPage> search(repositories::AuditQuery query) const override {
        repositories::AuditPage page;
        for (const auto& entry : entries) {
            if (!query.action.empty() && entry.action != query.action) {
                continue;
            }
            if (!query.actorUserId.empty() && entry.actorUserId != query.actorUserId) {
                continue;
            }
            page.items.push_back(entry);
        }
        page.total = static_cast<int64_t>(page.items.size());
        co_return page;
    }
};

} // namespace launcher::testing
