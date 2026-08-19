#include "services/AccountService.h"

#include <spdlog/spdlog.h>

#include <utility>

#include "common/Logging.h"
#include "domain/AuditEntry.h"
#include "domain/Role.h"
#include "domain/User.h"
#include "domain/Validation.h"
#include "domain/ValidationRules.h"

namespace launcher::services {
namespace {

using common::ErrorCode;
using common::VoidResult;

namespace actions = launcher::domain::auditActions;
namespace entities = launcher::domain::auditEntities;
namespace permissions = launcher::domain::permissions;

/// The token was accepted, so the account behind it has to exist; if it does not, the session
/// outlived what it names and re-authenticating is the only sensible instruction.
constexpr const char* NO_SUCH_SESSION = "this session no longer belongs to an account";

} // namespace

AccountService::AccountService(const repositories::IUserRepository& users,
                               const repositories::IAccountRepository& accounts,
                               const repositories::IAdminUserRepository& administrators,
                               const IPasswordHasher& passwordHasher)
    : users_(users),
      accounts_(accounts),
      administrators_(administrators),
      passwordHasher_(passwordHasher) {}

drogon::Task<VoidResult> AccountService::erase(domain::Actor actor,
                                               EraseAccountCommand command) const {
    if (!domain::isUuid(actor.userId)) {
        co_return VoidResult::failure(ErrorCode::Unauthenticated, NO_SUCH_SESSION);
    }
    if (command.reason.size() > domain::MAX_ERASURE_REASON_LENGTH) {
        co_return VoidResult::failure(common::invalidInput(
            "reason must be at most " + std::to_string(domain::MAX_ERASURE_REASON_LENGTH) +
                " characters",
            domain::rules::ERASURE_REASON_TOO_LONG,
            {std::to_string(domain::MAX_ERASURE_REASON_LENGTH)}));
    }

    const auto user = co_await users_.findById(actor.userId);
    if (!user.has_value()) {
        co_return VoidResult::failure(ErrorCode::Unauthenticated, NO_SUCH_SESSION);
    }

    // A valid access token says who is asking; it does not say that the person asking is the
    // owner rather than somebody who walked up to an unlocked machine. For the one request with
    // no undo, the password is asked for again.
    if (!passwordHasher_.verify(command.password, user->passwordHash)) {
        co_return VoidResult::failure(ErrorCode::Unauthenticated, "the password is incorrect");
    }

    // The same rule the admin surface applies to deactivation and role changes (D37), and here
    // it matters more, because this one cannot be reversed by another operator. The permission
    // is read from the token, which can only be stale in the harmless direction: an account
    // that has *lost* the role is asked to hand it on first, and gets through on its next
    // refresh.
    if (actor.can(permissions::ADMIN_USERS_MANAGE)) {
        const auto others = co_await administrators_.countOtherHoldersOf(
            permissions::ADMIN_USERS_MANAGE, actor.userId);
        if (others == 0) {
            co_return VoidResult::failure(
                ErrorCode::Conflict,
                "this is the only active account that can manage users; grant another one first");
        }
    }

    repositories::ErasedIdentity identity;
    identity.email = domain::erasedEmailFor(user->id);
    identity.displayName = domain::ERASED_DISPLAY_NAME;
    identity.passwordHash = domain::ERASED_PASSWORD_HASH;
    identity.reason = domain::trim(command.reason);

    // Built into named locals before the suspension: a braced initialiser passed straight to a
    // coroutine is what GCC 13 crashes on, and by-value coroutine parameters want this anyway.
    domain::NewAuditEntry audit;
    audit.actorUserId = user->id;
    audit.action = actions::USER_ERASED;
    audit.entityType = entities::USER;
    audit.entityId = user->id;

    const bool erased = co_await accounts_.erase(user->id, std::move(identity), std::move(audit));
    if (!erased) {
        co_return VoidResult::failure(ErrorCode::Unauthenticated, NO_SUCH_SESSION);
    }

    // The id and nothing else: this line outlives the account, and an address in it would put
    // back exactly what the request was asking to remove.
    spdlog::info("erased account id={}", common::escapeJson(user->id));
    co_return VoidResult::success();
}

} // namespace launcher::services
