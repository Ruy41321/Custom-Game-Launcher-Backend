#include <gtest/gtest.h>

#include <drogon/utils/coroutine.h>

#include <algorithm>
#include <string>
#include <vector>

#include "common/Random.h"
#include "domain/Role.h"
#include "services/CatalogService.h"
#include "support/FakeCatalogRepositories.h"

namespace {

using launcher::common::ErrorCode;
using launcher::domain::Actor;
using launcher::domain::BuildPlatform;
using launcher::domain::BuildStage;
using launcher::domain::Game;
using launcher::domain::GameVisibility;
using launcher::services::CatalogService;
using launcher::services::CreateBuildCommand;
using launcher::services::CreateGameCommand;
using launcher::services::CreateVersionCommand;
using launcher::testing::FakeBuildRepository;
using launcher::testing::FakeGameRepository;
using launcher::testing::FakeGameVersionRepository;
using launcher::testing::FakeLibraryRepository;

namespace permissions = launcher::domain::permissions;

const std::string PUBLISHER = launcher::common::randomUuid();
const std::string PLAYER = launcher::common::randomUuid();

Actor player(std::string userId = PLAYER) {
    return Actor{std::move(userId),
                 {permissions::LIBRARY_READ,
                  permissions::LIBRARY_MANAGE,
                  permissions::GAME_READ,
                  permissions::GAME_DOWNLOAD}};
}

Actor publisher(std::string userId = PUBLISHER) {
    auto actor = player(std::move(userId));
    actor.permissions.push_back(permissions::GAME_PUBLISH);
    actor.permissions.push_back(permissions::BUILD_UPLOAD);
    return actor;
}

Actor administrator() {
    auto actor = publisher(launcher::common::randomUuid());
    actor.permissions.push_back(permissions::ADMIN_GAMES_MANAGE);
    return actor;
}

/// Keeps every fake alive for the whole test so the service's references stay valid.
struct CatalogFixture {
    CatalogFixture()
        : service(games, versions, builds, library) {}

    FakeGameRepository games;
    FakeGameVersionRepository versions;
    FakeBuildRepository builds;
    FakeLibraryRepository library;
    CatalogService service;

    Game seedGame(GameVisibility visibility, const std::string& owner = PUBLISHER) {
        Game game;
        game.slug = "seeded-" + launcher::common::randomUuid().substr(0, 8);
        game.title = "Seeded Game";
        game.publisherUserId = owner;
        game.visibility = visibility;
        const auto seeded = games.seed(game);
        library.knownGameIds.push_back(seeded.id);
        return seeded;
    }
};

CreateGameCommand newGame(std::string title = "My Great Game") {
    CreateGameCommand command;
    command.title = std::move(title);
    return command;
}

// ---------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------

TEST(CatalogServiceTest, CreatesAGameAndDerivesItsSlug) {
    CatalogFixture fixture;

    const auto created =
        drogon::sync_wait(fixture.service.createGame(publisher(), newGame("My Great Game")));

    ASSERT_TRUE(created.ok()) << created.error().detail;
    EXPECT_EQ(created.value().slug, "my-great-game");
    EXPECT_EQ(created.value().publisherUserId, PUBLISHER);
    EXPECT_EQ(created.value().visibility, GameVisibility::Draft)
        << "a new game must not be published by accident";
}

TEST(CatalogServiceTest, RefusesToCreateAGameWithoutThePublishPermission) {
    CatalogFixture fixture;

    const auto created = drogon::sync_wait(fixture.service.createGame(player(), newGame()));

    ASSERT_FALSE(created.ok());
    EXPECT_EQ(created.error().code, ErrorCode::Forbidden);
}

TEST(CatalogServiceTest, RejectsASlugThatIsAlreadyTaken) {
    CatalogFixture fixture;
    ASSERT_TRUE(drogon::sync_wait(fixture.service.createGame(publisher(), newGame())).ok());

    const auto second = drogon::sync_wait(fixture.service.createGame(publisher(), newGame()));

    ASSERT_FALSE(second.ok());
    EXPECT_EQ(second.error().code, ErrorCode::Conflict);
}

TEST(CatalogServiceTest, RejectsATitleWithNoUsableSlug) {
    CatalogFixture fixture;

    const auto created = drogon::sync_wait(fixture.service.createGame(publisher(), newGame("!!!")));

    ASSERT_FALSE(created.ok());
    EXPECT_EQ(created.error().code, ErrorCode::InvalidInput);
}

TEST(CatalogServiceTest, RejectsAMalformedReleaseDate) {
    CatalogFixture fixture;
    auto command = newGame();
    command.releaseDate = "30-04-2026";

    const auto created = drogon::sync_wait(fixture.service.createGame(publisher(), command));

    ASSERT_FALSE(created.ok());
    EXPECT_EQ(created.error().code, ErrorCode::InvalidInput);
}

// ---------------------------------------------------------------------------
// Ownership
// ---------------------------------------------------------------------------

TEST(CatalogServiceTest, RefusesToEditSomebodyElsesGame) {
    CatalogFixture fixture;
    const auto game = fixture.seedGame(GameVisibility::Public);

    launcher::domain::GameUpdate changes;
    changes.title = "Hijacked";
    const auto updated = drogon::sync_wait(
        fixture.service.updateGame(publisher(launcher::common::randomUuid()), game.id, changes));

    ASSERT_FALSE(updated.ok());
    EXPECT_EQ(updated.error().code, ErrorCode::Forbidden);
    EXPECT_EQ(fixture.games.games[0].title, "Seeded Game");
}

TEST(CatalogServiceTest, LetsAnAdministratorEditAnyGame) {
    CatalogFixture fixture;
    const auto game = fixture.seedGame(GameVisibility::Public);

    launcher::domain::GameUpdate changes;
    changes.title = "Moderated";
    const auto updated =
        drogon::sync_wait(fixture.service.updateGame(administrator(), game.id, changes));

    ASSERT_TRUE(updated.ok()) << updated.error().detail;
    EXPECT_EQ(updated.value().title, "Moderated");
}

TEST(CatalogServiceTest, LeavesOmittedFieldsAloneOnAPartialUpdate) {
    CatalogFixture fixture;
    auto game = fixture.seedGame(GameVisibility::Public);
    fixture.games.games[0].summary = "original summary";

    launcher::domain::GameUpdate changes;
    changes.title = "Renamed";
    const auto updated =
        drogon::sync_wait(fixture.service.updateGame(publisher(), game.id, changes));

    ASSERT_TRUE(updated.ok()) << updated.error().detail;
    EXPECT_EQ(updated.value().title, "Renamed");
    EXPECT_EQ(updated.value().summary, "original summary");
}

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

// A draft is an unreleased title. Reporting Forbidden would confirm it exists, so it is
// reported as absent instead.
TEST(CatalogServiceTest, HidesADraftFromEverybodyButItsPublisher) {
    CatalogFixture fixture;
    const auto draft = fixture.seedGame(GameVisibility::Draft);

    const auto stranger = drogon::sync_wait(fixture.service.gameDetail(player(), draft.id));
    const auto owner = drogon::sync_wait(fixture.service.gameDetail(publisher(), draft.id));

    ASSERT_FALSE(stranger.ok());
    EXPECT_EQ(stranger.error().code, ErrorCode::NotFound);
    EXPECT_TRUE(owner.ok()) << owner.error().detail;
}

TEST(CatalogServiceTest, ServesAnUnlistedGameToAnybodyHoldingItsIdentifier) {
    CatalogFixture fixture;
    const auto unlisted = fixture.seedGame(GameVisibility::Unlisted);

    const auto detail = drogon::sync_wait(fixture.service.gameDetail(player(), unlisted.id));

    EXPECT_TRUE(detail.ok()) << detail.error().detail;
}

TEST(CatalogServiceTest, ExploreShowsOnlyPublicGames) {
    CatalogFixture fixture;
    fixture.seedGame(GameVisibility::Public);
    fixture.seedGame(GameVisibility::Draft);
    fixture.seedGame(GameVisibility::Unlisted);

    const auto page =
        drogon::sync_wait(fixture.service.explore(player(), launcher::repositories::GameQuery{}));

    ASSERT_TRUE(page.ok()) << page.error().detail;
    ASSERT_EQ(page.value().items.size(), 1U);
    EXPECT_EQ(page.value().items[0].visibility, GameVisibility::Public);
}

// A crafted request must not be able to widen Explore into a listing of everyone's drafts.
TEST(CatalogServiceTest, ExploreIgnoresARequestForUnpublishedGames) {
    CatalogFixture fixture;
    fixture.seedGame(GameVisibility::Draft);

    launcher::repositories::GameQuery query;
    query.includeUnpublished = true;
    query.publisherUserId = PUBLISHER;
    const auto page = drogon::sync_wait(fixture.service.explore(publisher(), query));

    ASSERT_TRUE(page.ok());
    EXPECT_TRUE(page.value().items.empty());
}

TEST(CatalogServiceTest, ThePublisherDashboardShowsTheirOwnDrafts) {
    CatalogFixture fixture;
    fixture.seedGame(GameVisibility::Draft);
    fixture.seedGame(GameVisibility::Draft, launcher::common::randomUuid());

    const auto page = drogon::sync_wait(
        fixture.service.publishedByActor(publisher(), launcher::repositories::GameQuery{}));

    ASSERT_TRUE(page.ok()) << page.error().detail;
    ASSERT_EQ(page.value().items.size(), 1U);
    EXPECT_EQ(page.value().items[0].publisherUserId, PUBLISHER);
}

TEST(CatalogServiceTest, GameDetailHidesUnpublishedVersionsFromPlayers) {
    CatalogFixture fixture;
    const auto game = fixture.seedGame(GameVisibility::Public);

    launcher::domain::GameVersion published;
    published.gameId = game.id;
    published.semver = "1.0";
    published.publishedAt = "2026-01-01T00:00:00Z";
    fixture.versions.seed(published);

    launcher::domain::GameVersion draft;
    draft.gameId = game.id;
    draft.semver = "1.1";
    fixture.versions.seed(draft);

    const auto asPlayer = drogon::sync_wait(fixture.service.gameDetail(player(), game.id));
    const auto asOwner = drogon::sync_wait(fixture.service.gameDetail(publisher(), game.id));

    ASSERT_TRUE(asPlayer.ok());
    ASSERT_TRUE(asOwner.ok());
    EXPECT_EQ(asPlayer.value().versions.size(), 1U);
    EXPECT_EQ(asOwner.value().versions.size(), 2U);
}

// ---------------------------------------------------------------------------
// Versions and builds
// ---------------------------------------------------------------------------

TEST(CatalogServiceTest, CreatesAVersionOnAGameTheActorOwns) {
    CatalogFixture fixture;
    const auto game = fixture.seedGame(GameVisibility::Draft);

    CreateVersionCommand command;
    command.semver = "0.2.1";
    command.stage = BuildStage::Beta;
    const auto created =
        drogon::sync_wait(fixture.service.createVersion(publisher(), game.id, command));

    ASSERT_TRUE(created.ok()) << created.error().detail;
    EXPECT_EQ(created.value().semver, "0.2.1");
    EXPECT_EQ(created.value().stage, BuildStage::Beta);
    EXPECT_TRUE(created.value().publishedAt.empty());
}

TEST(CatalogServiceTest, RejectsAVersionWithAMalformedSemver) {
    CatalogFixture fixture;
    const auto game = fixture.seedGame(GameVisibility::Draft);

    CreateVersionCommand command;
    command.semver = "v1.2-beta";
    const auto created =
        drogon::sync_wait(fixture.service.createVersion(publisher(), game.id, command));

    ASSERT_FALSE(created.ok());
    EXPECT_EQ(created.error().code, ErrorCode::InvalidInput);
}

// Pairing a version belonging to one game with a game the caller does own would otherwise let
// a build be hung off somebody else's release.
TEST(CatalogServiceTest, RefusesABuildForAVersionOfAnotherGame) {
    CatalogFixture fixture;
    const auto mine = fixture.seedGame(GameVisibility::Draft);
    const auto theirs = fixture.seedGame(GameVisibility::Draft, launcher::common::randomUuid());

    launcher::domain::GameVersion foreign;
    foreign.gameId = theirs.id;
    foreign.semver = "1.0";
    const auto seeded = fixture.versions.seed(foreign);

    CreateBuildCommand command;
    command.versionId = seeded.id;
    command.platform = BuildPlatform::Windows;
    const auto created =
        drogon::sync_wait(fixture.service.createBuild(publisher(), mine.id, command));

    ASSERT_FALSE(created.ok());
    EXPECT_EQ(created.error().code, ErrorCode::NotFound);
}

TEST(CatalogServiceTest, RefusesABuildWithoutTheUploadPermission) {
    CatalogFixture fixture;
    const auto game = fixture.seedGame(GameVisibility::Draft);

    launcher::domain::GameVersion version;
    version.gameId = game.id;
    version.semver = "1.0";
    const auto seeded = fixture.versions.seed(version);

    auto actor = publisher();
    actor.permissions.erase(
        std::remove(actor.permissions.begin(), actor.permissions.end(), permissions::BUILD_UPLOAD),
        actor.permissions.end());

    CreateBuildCommand command;
    command.versionId = seeded.id;
    const auto created = drogon::sync_wait(fixture.service.createBuild(actor, game.id, command));

    ASSERT_FALSE(created.ok());
    EXPECT_EQ(created.error().code, ErrorCode::Forbidden);
}

// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------

TEST(CatalogServiceTest, AddsAVisibleGameToTheLibraryIdempotently) {
    CatalogFixture fixture;
    const auto game = fixture.seedGame(GameVisibility::Public);

    ASSERT_TRUE(drogon::sync_wait(fixture.service.addToLibrary(player(), game.id)).ok());
    ASSERT_TRUE(drogon::sync_wait(fixture.service.addToLibrary(player(), game.id)).ok());

    EXPECT_EQ(fixture.library.memberships.size(), 1U);
}

// Otherwise the library would become a way to confirm that an unreleased title exists.
TEST(CatalogServiceTest, RefusesToAddADraftBelongingToSomebodyElse) {
    CatalogFixture fixture;
    const auto draft = fixture.seedGame(GameVisibility::Draft);

    const auto added = drogon::sync_wait(fixture.service.addToLibrary(player(), draft.id));

    ASSERT_FALSE(added.ok());
    EXPECT_EQ(added.error().code, ErrorCode::NotFound);
    EXPECT_TRUE(fixture.library.memberships.empty());
}

TEST(CatalogServiceTest, ReportsRemovingAGameThatIsNotInTheLibrary) {
    CatalogFixture fixture;
    const auto game = fixture.seedGame(GameVisibility::Public);

    const auto removed = drogon::sync_wait(fixture.service.removeFromLibrary(player(), game.id));

    ASSERT_FALSE(removed.ok());
    EXPECT_EQ(removed.error().code, ErrorCode::NotFound);
}

TEST(CatalogServiceTest, RefusesLibraryChangesWithoutTheManagePermission) {
    CatalogFixture fixture;
    const auto game = fixture.seedGame(GameVisibility::Public);

    Actor readOnly{PLAYER, {permissions::LIBRARY_READ, permissions::GAME_READ}};
    const auto added = drogon::sync_wait(fixture.service.addToLibrary(readOnly, game.id));

    ASSERT_FALSE(added.ok());
    EXPECT_EQ(added.error().code, ErrorCode::Forbidden);
}

} // namespace
