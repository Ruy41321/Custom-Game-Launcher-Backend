#pragma once

#include <drogon/utils/coroutine.h>

#include <string>

#include "common/Result.h"
#include "domain/Actor.h"
#include "repositories/IAccountRepository.h"
#include "repositories/IAdminUserRepository.h"
#include "repositories/IUserRepository.h"
#include "services/PasswordHasher.h"

namespace launcher::services {

struct EraseAccountCommand {
    /// Re-authentication. The caller is already holding a valid access token; this is about
    /// whether the person at the keyboard is the account's owner, on the one request that
    /// cannot be undone.
    std::string password;
    /// Optional, free text, kept in `account_deletion_requests.reason`.
    std::string reason;
};

/// What an account may do to itself. Today: ask to be erased.
///
/// Erasure is **immediate and irreversible**, and it **anonymises** rather than deletes:
///
///   * the row survives, carrying a placeholder address, "Deleted account" and a password hash
///     nothing can verify against;
///   * every session and every pending verification or reset link is destroyed;
///   * the library is emptied;
///   * `download_events.user_id` goes null, so the analytics keep their counts and lose their
///     subject — which the schema anticipated from the first migration;
///   * published games, patch notes and audit entries stay, now attributed to an anonymous row.
///
/// The last point is not a preference. `games.publisher_user_id` is `ON DELETE RESTRICT`, so a
/// `DELETE` on a publisher would be refused by the database, and the builds under those games
/// are what other people's installed copies update from. A publisher who wants their titles
/// gone deletes them first: that is a separate, deliberate act, and it exists.
class AccountService {
  public:
    AccountService(const repositories::IUserRepository& users,
                   const repositories::IAccountRepository& accounts,
                   const repositories::IAdminUserRepository& administrators,
                   const IPasswordHasher& passwordHasher);

    drogon::Task<common::VoidResult> erase(domain::Actor actor, EraseAccountCommand command) const;

  private:
    const repositories::IUserRepository& users_;
    const repositories::IAccountRepository& accounts_;
    /// Consulted for one rule: the last operator who can manage users may not erase themselves,
    /// because nothing but the command line repairs an empty administrator list.
    const repositories::IAdminUserRepository& administrators_;
    const IPasswordHasher& passwordHasher_;
};

} // namespace launcher::services
