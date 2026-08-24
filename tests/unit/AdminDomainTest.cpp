#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "domain/Admin.h"
#include "domain/Role.h"

namespace {

using launcher::domain::Actor;
using launcher::domain::isAdministrativePermission;
using launcher::domain::mayUseAdminSurface;
namespace permissions = launcher::domain::permissions;

Actor actorHolding(std::vector<std::string> permissions) {
    return Actor{"11111111-1111-1111-1111-111111111111", std::move(permissions)};
}

TEST(AdministrativePermissionTest, RecognisesEveryAdminKeySeededByTheSchema) {
    for (const auto* key : {permissions::ADMIN_USERS_MANAGE,
                            permissions::ADMIN_ROLES_MANAGE,
                            permissions::ADMIN_GAMES_MANAGE,
                            permissions::ADMIN_SETTINGS_MANAGE}) {
        EXPECT_TRUE(isAdministrativePermission(key)) << key;
    }
}

TEST(AdministrativePermissionTest, RejectsEveryOrdinaryKey) {
    for (const auto* key : {permissions::LIBRARY_READ,
                            permissions::LIBRARY_MANAGE,
                            permissions::GAME_READ,
                            permissions::GAME_DOWNLOAD,
                            permissions::GAME_PUBLISH,
                            permissions::BUILD_UPLOAD,
                            permissions::PATCHNOTE_WRITE}) {
        EXPECT_FALSE(isAdministrativePermission(key)) << key;
    }
}

TEST(AdministrativePermissionTest, RequiresSomethingAfterThePrefix) {
    // The prefix is the rule, so it has to be a prefix *of* something. A permission literally
    // named "admin." would otherwise open the surface while granting nothing.
    EXPECT_FALSE(isAdministrativePermission("admin."));
    EXPECT_FALSE(isAdministrativePermission("admin"));
}

TEST(AdministrativePermissionTest, DoesNotMatchAKeyThatMerelyContainsThePrefix) {
    EXPECT_FALSE(isAdministrativePermission("game.admin.manage"));
    EXPECT_FALSE(isAdministrativePermission("superadmin.users.manage"));
}

TEST(AdminSurfaceTest, AdmitsAnOperatorHoldingASingleAdminPermission) {
    // Coarse on purpose: this decides whether signing in is possible, not what the session may
    // then do. The individual actions still ask for the permission each of them needs.
    EXPECT_TRUE(mayUseAdminSurface(actorHolding({permissions::ADMIN_USERS_MANAGE})));
}

TEST(AdminSurfaceTest, AdmitsAnOperatorWhoAlsoHoldsOrdinaryPermissions) {
    // The seeded `admin` role holds every permission there is, so this is the real shape of an
    // operator rather than a contrived one.
    EXPECT_TRUE(mayUseAdminSurface(actorHolding(
        {permissions::LIBRARY_READ, permissions::GAME_PUBLISH, permissions::ADMIN_GAMES_MANAGE})));
}

TEST(AdminSurfaceTest, RefusesAPublisher) {
    EXPECT_FALSE(mayUseAdminSurface(actorHolding(
        {permissions::GAME_PUBLISH, permissions::BUILD_UPLOAD, permissions::PATCHNOTE_WRITE})));
}

TEST(AdminSurfaceTest, RefusesAnActorHoldingNothing) {
    EXPECT_FALSE(mayUseAdminSurface(actorHolding({})));
}

} // namespace
