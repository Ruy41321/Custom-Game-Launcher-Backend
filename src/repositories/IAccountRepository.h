#pragma once

#include <drogon/utils/coroutine.h>

#include <string>

#include "domain/AuditEntry.h"

namespace launcher::repositories {

/// The pseudonymous values an erased account's row is left holding, plus what the account said
/// on its way out. Built by the service from `domain::erasedEmailFor` and friends, so the
/// repository stores what it is given rather than inventing an identity of its own.
struct ErasedIdentity {
    std::string email;
    std::string displayName;
    std::string passwordHash;
    std::string reason;
};

/// What an account can do to itself. Today that is exactly one thing: ask to be erased.
///
/// Separate from IUserRepository for the reason D36 gives for IAdminUserRepository being
/// separate — this mutation takes the audit entry that describes it and writes it in the *same*
/// statement, and registration and a password reset have no business carrying one. An erasure
/// is precisely the change where a record written afterwards, and failing, would leave
/// something irreversible that nobody can attribute.
///
/// Every parameter is taken by value: a coroutine's reference parameters dangle as soon as it
/// first suspends.
class IAccountRepository {
  public:
    virtual ~IAccountRepository() = default;

    /// Anonymises the account, ends every session it holds, empties its library and detaches it
    /// from the download events it produced — all in one statement, alongside the audit entry
    /// and the `account_deletion_requests` row that records the request was carried out.
    ///
    /// False when no account had that id, or when it was already erased. Both are the same
    /// answer to the caller and neither records anything.
    virtual drogon::Task<bool>
    erase(std::string userId, ErasedIdentity identity, domain::NewAuditEntry audit) const = 0;
};

} // namespace launcher::repositories
