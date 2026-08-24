#include <gtest/gtest.h>

#include <drogon/utils/coroutine.h>

#include <string>

#include "common/Random.h"
#include "domain/AuditEntry.h"
#include "domain/Role.h"
#include "services/AdminUserService.h"
#include "services/PasswordHasher.h"
#include "support/FakeAdminRepositories.h"

namespace {

using launcher::common::ErrorCode;
using launcher::domain::Actor;
using launcher::services::AdminUserService;
using launcher::services::Argon2idPasswordHasher;
using launcher::testing::FakeAdminUserRepository;
using launcher::testing::FakeAuditRepository;

namespace permissions = launcher::domain::permissions;
namespace actions = launcher::domain::auditActions;

const std::string OPERATOR = launcher::common::randomUuid();
const std::string SECOND_OPERATOR = launcher::common::randomUuid();
const std::string PLAYER = launcher::common::randomUuid();

Actor operatorActor(std::string userId = OPERATOR) {
    return Actor{std::move(userId),
                 {permissions::ADMIN_USERS_MANAGE,
                  permissions::ADMIN_ROLES_MANAGE,
                  permissions::ADMIN_SETTINGS_MANAGE}};
}

Actor publisher() {
    return Actor{PLAYER, {permissions::GAME_PUBLISH, permissions::BUILD_UPLOAD}};
}

struct AdminFixture {
    AdminFixture()
        : service(users, audit, hasher) {
        users.permissionsByRole["player"] = {permissions::GAME_READ};
        users.permissionsByRole["dev"] = {permissions::GAME_PUBLISH};
        users.permissionsByRole["admin"] = {permissions::ADMIN_USERS_MANAGE,
                                            permissions::ADMIN_ROLES_MANAGE};

        users.seed(OPERATOR, "operator@example.test");
        users.grant(OPERATOR, "admin");
        users.seed(PLAYER, "player@example.test");
        users.grant(PLAYER, "player");
    }

    /// A second operator, so the last-administrator guard is out of the way for tests that are
    /// about something else.
    void seedSecondOperator() {
        users.seed(SECOND_OPERATOR, "second@example.test");
        users.grant(SECOND_OPERATOR, "admin");
    }

    FakeAdminUserRepository users;
    FakeAuditRepository audit;
    /// The weakest Argon2id libsodium accepts. These tests hash once per temporary password
    /// and care about nothing but that the result verifies, so the interactive profile would
    /// be a second of CPU spent proving something no assertion here looks at.
    Argon2idPasswordHasher hasher{
        launcher::services::PasswordHashingSettings{1, 8U * 1024U * 1024U}};
    AdminUserService service;
};

// ---------------------------------------------------------------------------
// Authorization
// ---------------------------------------------------------------------------

TEST(AdminUserServiceTest, RefusesEveryOperationToACallerWithoutTheirPermission) {
    AdminFixture fixture;

    const auto listed = drogon::sync_wait(fixture.service.list(publisher(), {}));
    EXPECT_FALSE(listed.ok());
    EXPECT_EQ(listed.error().code, ErrorCode::Forbidden);

    const auto quota = drogon::sync_wait(fixture.service.setUploadQuota(publisher(), PLAYER, 1));
    EXPECT_EQ(quota.error().code, ErrorCode::Forbidden);

    const auto granted = drogon::sync_wait(fixture.service.grantRole(publisher(), PLAYER, "dev"));
    EXPECT_EQ(granted.error().code, ErrorCode::Forbidden);

    const auto trail = drogon::sync_wait(fixture.service.listAudit(publisher(), {}));
    EXPECT_EQ(trail.error().code, ErrorCode::Forbidden);

    // A refused call must not have reached the repository at all.
    EXPECT_TRUE(fixture.users.recorded.empty());
}

TEST(AdminUserServiceTest, SeparatesManagingUsersFromManagingRoles) {
    AdminFixture fixture;
    fixture.seedSecondOperator();

    // Holding one administrative permission opens the surface but not every action on it,
    // which is the whole reason the operator filter is coarse and the services are not.
    Actor quotaOnly{OPERATOR, {permissions::ADMIN_USERS_MANAGE}};

    EXPECT_TRUE(drogon::sync_wait(fixture.service.setUploadQuota(quotaOnly, PLAYER, 4096)).ok());
    EXPECT_EQ(drogon::sync_wait(fixture.service.grantRole(quotaOnly, PLAYER, "dev")).error().code,
              ErrorCode::Forbidden);
}

// ---------------------------------------------------------------------------
// Quotas
// ---------------------------------------------------------------------------

TEST(AdminUserServiceTest, ChangesAQuotaAndRecordsTheNewValue) {
    AdminFixture fixture;

    const auto updated =
        drogon::sync_wait(fixture.service.setUploadQuota(operatorActor(), PLAYER, 9001));

    ASSERT_TRUE(updated.ok()) << updated.error().detail;
    EXPECT_EQ(updated.value().user.uploadQuotaBytes, 9001);

    ASSERT_EQ(fixture.users.recorded.size(), 1U);
    const auto& entry = fixture.users.recorded[0];
    EXPECT_EQ(entry.action, actions::USER_QUOTA_CHANGED);
    EXPECT_EQ(entry.actorUserId, OPERATOR);
    EXPECT_EQ(entry.entityId, PLAYER);
    ASSERT_EQ(entry.metadata.size(), 1U);
    EXPECT_EQ(entry.metadata[0].first, "quotaBytes");
    EXPECT_EQ(entry.metadata[0].second, "9001");
}

TEST(AdminUserServiceTest, RefusesANegativeQuotaAndRecordsNothing) {
    AdminFixture fixture;

    const auto updated =
        drogon::sync_wait(fixture.service.setUploadQuota(operatorActor(), PLAYER, -1));

    EXPECT_EQ(updated.error().code, ErrorCode::InvalidInput);
    EXPECT_TRUE(fixture.users.recorded.empty());
}

TEST(AdminUserServiceTest, AllowsAQuotaBelowWhatIsAlreadyStored) {
    AdminFixture fixture;
    fixture.users.accounts[PLAYER].user.uploadUsedBytes = 5000;

    // Deliberately permitted: it stops further uploads without deleting anything, which is
    // what an operator reaches for when an account is filling the disk.
    const auto updated =
        drogon::sync_wait(fixture.service.setUploadQuota(operatorActor(), PLAYER, 10));

    ASSERT_TRUE(updated.ok()) << updated.error().detail;
    EXPECT_EQ(updated.value().user.uploadQuotaBytes, 10);
}

TEST(AdminUserServiceTest, ReportsAMalformedIdAsAMissingAccount) {
    AdminFixture fixture;

    // Not a server error: without this the value reaches a `$n::uuid` comparison and
    // PostgreSQL raises, turning a mistyped address bar into a 500.
    const auto found = drogon::sync_wait(fixture.service.find(operatorActor(), "not-a-uuid"));

    EXPECT_EQ(found.error().code, ErrorCode::NotFound);
}

// ---------------------------------------------------------------------------
// Roles
// ---------------------------------------------------------------------------

TEST(AdminUserServiceTest, GrantsARoleAndRecordsWhichOne) {
    AdminFixture fixture;

    const auto updated =
        drogon::sync_wait(fixture.service.grantRole(operatorActor(), PLAYER, "dev"));

    ASSERT_TRUE(updated.ok()) << updated.error().detail;
    EXPECT_NE(std::find(updated.value().roles.begin(), updated.value().roles.end(), "dev"),
              updated.value().roles.end());

    ASSERT_EQ(fixture.users.recorded.size(), 1U);
    EXPECT_EQ(fixture.users.recorded[0].action, actions::ROLE_GRANTED);
    EXPECT_EQ(fixture.users.recorded[0].metadata[0].second, "dev");
}

TEST(AdminUserServiceTest, GrantingARoleTwiceSucceedsAndRecordsOnce) {
    AdminFixture fixture;

    EXPECT_TRUE(drogon::sync_wait(fixture.service.grantRole(operatorActor(), PLAYER, "dev")).ok());
    // Idempotent on purpose: the end state is the same, and failing here would make retrying a
    // lost response dangerous for no reason. The trail records what changed, not what was asked.
    EXPECT_TRUE(drogon::sync_wait(fixture.service.grantRole(operatorActor(), PLAYER, "dev")).ok());

    EXPECT_EQ(fixture.users.recorded.size(), 1U);
}

TEST(AdminUserServiceTest, RejectsAnUnknownRole) {
    AdminFixture fixture;

    const auto updated =
        drogon::sync_wait(fixture.service.grantRole(operatorActor(), PLAYER, "wizard"));

    EXPECT_EQ(updated.error().code, ErrorCode::InvalidInput);
    EXPECT_TRUE(fixture.users.recorded.empty());
}

TEST(AdminUserServiceTest, RevokesARole) {
    AdminFixture fixture;
    fixture.seedSecondOperator();

    const auto updated =
        drogon::sync_wait(fixture.service.revokeRole(operatorActor(), PLAYER, "player"));

    ASSERT_TRUE(updated.ok()) << updated.error().detail;
    EXPECT_TRUE(updated.value().roles.empty());
    EXPECT_EQ(fixture.users.recorded[0].action, actions::ROLE_REVOKED);
}

// ---------------------------------------------------------------------------
// The way back in
// ---------------------------------------------------------------------------

TEST(AdminUserServiceTest, RefusesToRevokeARoleFromTheLastAdministrator) {
    AdminFixture fixture;

    // There is no endpoint that repairs an empty administrator list — only the command line —
    // so the surface refuses to create one.
    const auto updated =
        drogon::sync_wait(fixture.service.revokeRole(operatorActor(), OPERATOR, "admin"));

    EXPECT_EQ(updated.error().code, ErrorCode::Conflict);
    EXPECT_TRUE(fixture.users.recorded.empty());
}

TEST(AdminUserServiceTest, AllowsRevokingOnceAnotherAdministratorExists) {
    AdminFixture fixture;
    fixture.seedSecondOperator();

    EXPECT_TRUE(
        drogon::sync_wait(fixture.service.revokeRole(operatorActor(), OPERATOR, "admin")).ok());
}

TEST(AdminUserServiceTest, RefusesToDeactivateTheLastAdministrator) {
    AdminFixture fixture;
    fixture.seedSecondOperator();
    // Deactivated accounts do not count as a way back in, so the second operator being
    // disabled leaves the first one alone again.
    fixture.users.accounts[SECOND_OPERATOR].user.isActive = false;

    const auto updated = drogon::sync_wait(
        fixture.service.setActive(operatorActor(SECOND_OPERATOR), OPERATOR, false));

    EXPECT_EQ(updated.error().code, ErrorCode::Conflict);
}

TEST(AdminUserServiceTest, RefusesToDeactivateYourOwnAccount) {
    AdminFixture fixture;
    fixture.seedSecondOperator();

    const auto updated =
        drogon::sync_wait(fixture.service.setActive(operatorActor(), OPERATOR, false));

    EXPECT_EQ(updated.error().code, ErrorCode::InvalidInput);
    EXPECT_TRUE(fixture.users.recorded.empty());
}

TEST(AdminUserServiceTest, DeactivatesAnOrdinaryAccountAndRecordsIt) {
    AdminFixture fixture;

    const auto updated =
        drogon::sync_wait(fixture.service.setActive(operatorActor(), PLAYER, false));

    ASSERT_TRUE(updated.ok()) << updated.error().detail;
    EXPECT_FALSE(updated.value().user.isActive);
    EXPECT_EQ(fixture.users.recorded[0].action, actions::USER_DEACTIVATED);
}

TEST(AdminUserServiceTest, ReactivatingIsNeverGuarded) {
    AdminFixture fixture;
    fixture.users.accounts[PLAYER].user.isActive = false;

    // The guard exists to stop the administrator list emptying. Switching an account back on
    // cannot do that, so it is never refused.
    const auto updated =
        drogon::sync_wait(fixture.service.setActive(operatorActor(), PLAYER, true));

    ASSERT_TRUE(updated.ok()) << updated.error().detail;
    EXPECT_EQ(fixture.users.recorded[0].action, actions::USER_ACTIVATED);
}

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------

TEST(AdminUserServiceTest, ClampsAnAbsurdPageSize) {
    AdminFixture fixture;

    launcher::repositories::AdminUserQuery query;
    query.limit = 100000;
    query.offset = -5;

    EXPECT_TRUE(drogon::sync_wait(fixture.service.list(operatorActor(), query)).ok());
}

// ---------------------------------------------------------------------------
// One-time passwords — the way back in where no mail transport exists
// ---------------------------------------------------------------------------

TEST(AdminUserServiceTest, HandsOutAPasswordThatWorksAndDemandsItBeReplaced) {
    AdminFixture fixture;

    const auto issued =
        drogon::sync_wait(fixture.service.setTemporaryPassword(operatorActor(), PLAYER));

    ASSERT_TRUE(issued.ok()) << issued.error().detail;
    EXPECT_FALSE(issued.value().password.empty());
    EXPECT_TRUE(issued.value().user.user.passwordChangeRequired);

    // What was stored is a hash of what was handed over, and nothing else verifies against it.
    const auto& stored = fixture.users.accounts[PLAYER].user;
    EXPECT_TRUE(fixture.hasher.verify(issued.value().password, stored.passwordHash));
    EXPECT_FALSE(fixture.hasher.verify("something else entirely", stored.passwordHash));
}

// It is read out loud or copied off a note, so it may not contain the characters people
// disagree about — and it must not be the same one twice.
TEST(AdminUserServiceTest, ThePasswordIsReadableAndNeverRepeats) {
    AdminFixture fixture;

    const auto first =
        drogon::sync_wait(fixture.service.setTemporaryPassword(operatorActor(), PLAYER));
    const auto second =
        drogon::sync_wait(fixture.service.setTemporaryPassword(operatorActor(), PLAYER));

    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_NE(first.value().password, second.value().password);

    for (const char character : first.value().password) {
        EXPECT_EQ(std::string("l1o0O").find(character), std::string::npos)
            << "a character nobody can dictate ended up in a password somebody has to dictate";
    }
}

TEST(AdminUserServiceTest, RecordsWhoDidItAndNotWhatTheyHandedOver) {
    AdminFixture fixture;

    const auto issued =
        drogon::sync_wait(fixture.service.setTemporaryPassword(operatorActor(), PLAYER));
    ASSERT_TRUE(issued.ok());

    ASSERT_EQ(fixture.users.recorded.size(), 1U);
    const auto& entry = fixture.users.recorded[0];
    EXPECT_EQ(entry.action, actions::USER_TEMPORARY_PASSWORD_SET);
    EXPECT_EQ(entry.actorUserId, OPERATOR);
    EXPECT_EQ(entry.entityId, PLAYER);

    // The credential exists for as long as it takes to read it out of one response. An audit
    // trail carrying it would be the place it outlived that.
    for (const auto& [key, value] : entry.metadata) {
        EXPECT_NE(value, issued.value().password);
    }
}

// The console has no password-change route, and the flag refuses everything else — so an
// operator doing this to themselves locks themselves out of the surface they are standing on.
TEST(AdminUserServiceTest, AnOperatorCannotDoThisToTheirOwnAccount) {
    AdminFixture fixture;

    const auto refused =
        drogon::sync_wait(fixture.service.setTemporaryPassword(operatorActor(), OPERATOR));

    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidInput);
    EXPECT_FALSE(fixture.users.accounts[OPERATOR].user.passwordChangeRequired);
    EXPECT_TRUE(fixture.users.recorded.empty());
}

TEST(AdminUserServiceTest, RefusesTheTemporaryPasswordToACallerWithoutThePermission) {
    AdminFixture fixture;

    const auto refused =
        drogon::sync_wait(fixture.service.setTemporaryPassword(publisher(), PLAYER));

    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, ErrorCode::Forbidden);
    EXPECT_FALSE(fixture.users.accounts[PLAYER].user.passwordChangeRequired);
    EXPECT_TRUE(fixture.users.recorded.empty());
}

TEST(AdminUserServiceTest, AnAccountThatIsNotThereIsANotFoundAndNotAServerError) {
    AdminFixture fixture;

    const auto missing = drogon::sync_wait(
        fixture.service.setTemporaryPassword(operatorActor(), launcher::common::randomUuid()));
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);

    // A malformed id never reaches a $n::uuid comparison, which would raise rather than miss.
    const auto malformed =
        drogon::sync_wait(fixture.service.setTemporaryPassword(operatorActor(), "not-a-uuid"));
    EXPECT_EQ(malformed.error().code, ErrorCode::NotFound);

    EXPECT_TRUE(fixture.users.recorded.empty());
}

TEST(AdminUserServiceTest, ReadsTheAuditTrail) {
    AdminFixture fixture;

    launcher::domain::AuditEntry entry;
    entry.action = actions::USER_QUOTA_CHANGED;
    entry.actorUserId = OPERATOR;
    fixture.audit.entries.push_back(entry);

    const auto trail = drogon::sync_wait(fixture.service.listAudit(operatorActor(), {}));

    ASSERT_TRUE(trail.ok()) << trail.error().detail;
    EXPECT_EQ(trail.value().total, 1);
}

} // namespace
