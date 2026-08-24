#include <gtest/gtest.h>

#include <drogon/utils/coroutine.h>

#include <string>

#include "common/Random.h"
#include "domain/Role.h"
#include "domain/User.h"
#include "services/AccountService.h"
#include "support/FakeAdminRepositories.h"
#include "support/FakeRepositories.h"

namespace {

using launcher::common::ErrorCode;
using launcher::domain::Actor;
using launcher::domain::User;
using launcher::services::AccountService;
using launcher::services::Argon2idPasswordHasher;
using launcher::services::EraseAccountCommand;
using launcher::services::PasswordHashingSettings;
using launcher::testing::FakeAccountRepository;
using launcher::testing::FakeAdminUserRepository;
using launcher::testing::FakeUserRepository;

namespace actions = launcher::domain::auditActions;
namespace permissions = launcher::domain::permissions;

constexpr const char* VALID_PASSWORD = "correct horse battery staple";

/// Keeps every fake alive for the whole test so the service's references stay valid.
struct AccountFixture {
    AccountFixture()
        : hasher(PasswordHashingSettings{2, 64U * 1024U * 1024U}),
          service(users, accounts, administrators, hasher) {
        accounts.users = &users;
    }

    FakeUserRepository users;
    FakeAccountRepository accounts;
    FakeAdminUserRepository administrators;
    Argon2idPasswordHasher hasher;
    AccountService service;

    User seedAccount(const std::string& email = "leaving@example.com") {
        User user;
        user.id = launcher::common::randomUuid();
        user.email = email;
        user.displayName = "Leaving";
        user.passwordHash = hasher.hash(VALID_PASSWORD).value();
        user.emailVerified = true;
        user.isActive = true;
        return users.seed(user);
    }

    const User* reread(const std::string& id) const {
        for (const auto& user : users.users) {
            if (user.id == id) {
                return &user;
            }
        }
        return nullptr;
    }
};

Actor player(const User& user) {
    return Actor{user.id, {permissions::LIBRARY_READ, permissions::GAME_READ}};
}

Actor operatorFor(const User& user) {
    auto actor = player(user);
    actor.permissions.push_back(permissions::ADMIN_USERS_MANAGE);
    return actor;
}

EraseAccountCommand withPassword(std::string password = VALID_PASSWORD) {
    EraseAccountCommand command;
    command.password = std::move(password);
    return command;
}

// ---------------------------------------------------------------------------
// What an erasure does
// ---------------------------------------------------------------------------

TEST(AccountServiceTest, AnonymisesTheAccountRatherThanDeletingIt) {
    AccountFixture fixture;
    const auto account = fixture.seedAccount();

    const auto erased = drogon::sync_wait(fixture.service.erase(player(account), withPassword()));

    ASSERT_TRUE(erased.ok()) << erased.error().detail;
    const auto* after = fixture.reread(account.id);
    // The row survives on purpose: games.publisher_user_id is ON DELETE RESTRICT, and the
    // content pointing at this row outlives the person who made it.
    ASSERT_NE(after, nullptr);
    EXPECT_NE(after->email, account.email);
    EXPECT_EQ(after->displayName, launcher::domain::ERASED_DISPLAY_NAME);
    EXPECT_FALSE(after->isActive);
    EXPECT_FALSE(after->emailVerified);
}

TEST(AccountServiceTest, LeavesNoPasswordThatCanEverOpenTheAccountAgain) {
    AccountFixture fixture;
    const auto account = fixture.seedAccount();

    ASSERT_TRUE(drogon::sync_wait(fixture.service.erase(player(account), withPassword())).ok());

    const auto* after = fixture.reread(account.id);
    ASSERT_NE(after, nullptr);
    EXPECT_NE(after->passwordHash, account.passwordHash);
    EXPECT_FALSE(fixture.hasher.verify(VALID_PASSWORD, after->passwordHash));
    EXPECT_FALSE(fixture.hasher.verify(launcher::domain::ERASED_PASSWORD_HASH, after->passwordHash))
        << "the placeholder must not be a password either";
}

TEST(AccountServiceTest, TheReplacementAddressIsUniquePerAccount) {
    AccountFixture fixture;
    const auto first = fixture.seedAccount("first@example.com");
    const auto second = fixture.seedAccount("second@example.com");

    ASSERT_TRUE(drogon::sync_wait(fixture.service.erase(player(first), withPassword())).ok());
    ASSERT_TRUE(drogon::sync_wait(fixture.service.erase(player(second), withPassword())).ok());

    // users.email is UNIQUE, so a fixed placeholder would make the second erasure on a
    // deployment fail on the index rather than succeed.
    EXPECT_NE(fixture.reread(first.id)->email, fixture.reread(second.id)->email);
}

TEST(AccountServiceTest, RecordsWhoErasedWhatInTheSameBreath) {
    AccountFixture fixture;
    const auto account = fixture.seedAccount();

    ASSERT_TRUE(drogon::sync_wait(fixture.service.erase(player(account), withPassword())).ok());

    ASSERT_EQ(fixture.accounts.recorded.size(), 1U);
    EXPECT_EQ(fixture.accounts.recorded[0].action, actions::USER_ERASED);
    EXPECT_EQ(fixture.accounts.recorded[0].actorUserId, account.id);
    EXPECT_EQ(fixture.accounts.recorded[0].entityId, account.id);
}

TEST(AccountServiceTest, AnErasureThatFailsRecordsNothing) {
    AccountFixture fixture;
    const auto account = fixture.seedAccount();
    fixture.accounts.refuse = true;

    const auto erased = drogon::sync_wait(fixture.service.erase(player(account), withPassword()));

    ASSERT_FALSE(erased.ok());
    EXPECT_TRUE(fixture.accounts.recorded.empty())
        << "an audit trail that can record a change which did not happen is not one";
    EXPECT_EQ(fixture.reread(account.id)->email, account.email);
}

TEST(AccountServiceTest, KeepsTheReasonTheAccountGave) {
    AccountFixture fixture;
    const auto account = fixture.seedAccount();

    auto command = withPassword();
    command.reason = "  moving on  ";
    ASSERT_TRUE(drogon::sync_wait(fixture.service.erase(player(account), command)).ok());

    ASSERT_EQ(fixture.accounts.reasons.size(), 1U);
    EXPECT_EQ(fixture.accounts.reasons[0], "moving on");
}

TEST(AccountServiceTest, RefusesAnOverlongReasonBeforeChangingAnything) {
    AccountFixture fixture;
    const auto account = fixture.seedAccount();

    auto command = withPassword();
    command.reason = std::string(launcher::domain::MAX_ERASURE_REASON_LENGTH + 1, 'x');
    const auto erased = drogon::sync_wait(fixture.service.erase(player(account), command));

    ASSERT_FALSE(erased.ok());
    EXPECT_EQ(erased.error().code, ErrorCode::InvalidInput);
    EXPECT_EQ(fixture.reread(account.id)->email, account.email);
}

// ---------------------------------------------------------------------------
// Who may ask
// ---------------------------------------------------------------------------

TEST(AccountServiceTest, RefusesWithoutTheCurrentPassword) {
    AccountFixture fixture;
    const auto account = fixture.seedAccount();

    const auto erased =
        drogon::sync_wait(fixture.service.erase(player(account), withPassword("not the password")));

    ASSERT_FALSE(erased.ok());
    // A valid token says who is asking, not that the owner is the one at the keyboard.
    EXPECT_EQ(erased.error().code, ErrorCode::Unauthenticated);
    EXPECT_EQ(fixture.reread(account.id)->email, account.email);
}

TEST(AccountServiceTest, ASecondErasureChangesNothingAndRecordsNothing) {
    AccountFixture fixture;
    const auto account = fixture.seedAccount();
    ASSERT_TRUE(drogon::sync_wait(fixture.service.erase(player(account), withPassword())).ok());

    const auto again = drogon::sync_wait(fixture.service.erase(player(account), withPassword()));

    ASSERT_FALSE(again.ok());
    EXPECT_EQ(fixture.accounts.recorded.size(), 1U);
}

TEST(AccountServiceTest, ReportsASessionWhoseAccountIsGone) {
    AccountFixture fixture;

    Actor stranger{launcher::common::randomUuid(), {permissions::GAME_READ}};
    const auto erased = drogon::sync_wait(fixture.service.erase(stranger, withPassword()));

    ASSERT_FALSE(erased.ok());
    EXPECT_EQ(erased.error().code, ErrorCode::Unauthenticated);
}

TEST(AccountServiceTest, RefusesToEraseTheOnlyRemainingOperator) {
    AccountFixture fixture;
    const auto account = fixture.seedAccount("only.admin@example.com");
    fixture.administrators.permissionsByRole["admin"] = {permissions::ADMIN_USERS_MANAGE};
    fixture.administrators.seed(account.id, account.email);
    fixture.administrators.grant(account.id, "admin");

    const auto erased =
        drogon::sync_wait(fixture.service.erase(operatorFor(account), withPassword()));

    // Nothing but the command line repairs an empty administrator list, and unlike a revoked
    // role this one cannot be handed back.
    ASSERT_FALSE(erased.ok());
    EXPECT_EQ(erased.error().code, ErrorCode::Conflict);
    EXPECT_EQ(fixture.reread(account.id)->email, account.email);
}

TEST(AccountServiceTest, LetsAnOperatorLeaveOnceAnotherOneCanManageUsers) {
    AccountFixture fixture;
    const auto leaving = fixture.seedAccount("leaving.admin@example.com");
    const auto staying = fixture.seedAccount("staying.admin@example.com");
    fixture.administrators.permissionsByRole["admin"] = {permissions::ADMIN_USERS_MANAGE};
    fixture.administrators.seed(leaving.id, leaving.email);
    fixture.administrators.grant(leaving.id, "admin");
    fixture.administrators.seed(staying.id, staying.email);
    fixture.administrators.grant(staying.id, "admin");

    const auto erased =
        drogon::sync_wait(fixture.service.erase(operatorFor(leaving), withPassword()));

    EXPECT_TRUE(erased.ok()) << erased.error().detail;
}

} // namespace
