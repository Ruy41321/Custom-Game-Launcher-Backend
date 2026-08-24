#include <gtest/gtest.h>

#include <drogon/utils/coroutine.h>

#include <string>

#include "common/Random.h"
#include "domain/Role.h"
#include "services/PatchNoteService.h"
#include "support/FakeCatalogRepositories.h"

namespace {

using launcher::common::ErrorCode;
using launcher::domain::Actor;
using launcher::domain::Game;
using launcher::domain::GameVisibility;
using launcher::domain::PatchNoteUpdate;
using launcher::services::CreatePatchNoteCommand;
using launcher::services::PatchNoteService;
using launcher::testing::FakeGameRepository;
using launcher::testing::FakeGameVersionRepository;
using launcher::testing::FakePatchNoteRepository;

namespace permissions = launcher::domain::permissions;

const std::string PUBLISHER = launcher::common::randomUuid();
const std::string PLAYER = launcher::common::randomUuid();

Actor player(std::string userId = PLAYER) {
    return Actor{std::move(userId), {permissions::GAME_READ, permissions::LIBRARY_READ}};
}

Actor publisher(std::string userId = PUBLISHER) {
    auto actor = player(std::move(userId));
    actor.permissions.push_back(permissions::GAME_PUBLISH);
    return actor;
}

Actor administrator() {
    auto actor = publisher(launcher::common::randomUuid());
    actor.permissions.push_back(permissions::ADMIN_GAMES_MANAGE);
    return actor;
}

struct PatchNoteFixture {
    PatchNoteFixture()
        : service(games, versions, notes) {}

    FakeGameRepository games;
    FakeGameVersionRepository versions;
    FakePatchNoteRepository notes;
    PatchNoteService service;

    Game seedGame(GameVisibility visibility = GameVisibility::Public,
                  const std::string& owner = PUBLISHER) {
        Game game;
        game.slug = "seeded-" + launcher::common::randomUuid().substr(0, 8);
        game.title = "Seeded Game";
        game.publisherUserId = owner;
        game.visibility = visibility;
        return games.seed(game);
    }

    std::string seedVersion(const std::string& gameId) {
        launcher::domain::GameVersion version;
        version.gameId = gameId;
        version.semver = "1.0.0";
        version.publishedAt = "2026-01-01T00:00:00Z";
        return versions.seed(version).id;
    }
};

CreatePatchNoteCommand entry(std::string title = "What we shipped", bool publish = true) {
    CreatePatchNoteCommand command;
    command.title = std::move(title);
    command.bodyMarkdown = "## Fixed\n\nThe thing that was broken.";
    command.publish = publish;
    return command;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

TEST(PatchNoteServiceTest, PublishesANoteOnAGameTheActorOwns) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame();

    const auto created = drogon::sync_wait(fixture.service.create(publisher(), game.id, entry()));

    ASSERT_TRUE(created.ok()) << created.error().detail;
    EXPECT_EQ(created.value().title, "What we shipped");
    EXPECT_TRUE(created.value().published());
    EXPECT_EQ(created.value().authorUserId, PUBLISHER);
}

TEST(PatchNoteServiceTest, WritesADraftThatIsNotPublished) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame();

    const auto created = drogon::sync_wait(
        fixture.service.create(publisher(), game.id, entry("Still writing", false)));

    ASSERT_TRUE(created.ok()) << created.error().detail;
    // A note can be written before the build it talks about exists, which is the whole reason
    // it has a publication state of its own.
    EXPECT_FALSE(created.value().published());
}

TEST(PatchNoteServiceTest, RejectsAnEmptyOrOverlongTitle) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame();

    const auto blank =
        drogon::sync_wait(fixture.service.create(publisher(), game.id, entry("   ")));
    const auto overlong = drogon::sync_wait(fixture.service.create(
        publisher(),
        game.id,
        entry(std::string(launcher::domain::MAX_PATCH_NOTE_TITLE_LENGTH + 1, 'a'))));

    ASSERT_FALSE(blank.ok());
    EXPECT_EQ(blank.error().code, ErrorCode::InvalidInput);
    ASSERT_FALSE(overlong.ok());
    EXPECT_EQ(overlong.error().code, ErrorCode::InvalidInput);
}

TEST(PatchNoteServiceTest, RejectsAnOverlongBody) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame();

    auto command = entry();
    command.bodyMarkdown = std::string(launcher::domain::MAX_PATCH_NOTE_BODY_LENGTH + 1, 'a');
    const auto created = drogon::sync_wait(fixture.service.create(publisher(), game.id, command));

    ASSERT_FALSE(created.ok());
    EXPECT_EQ(created.error().code, ErrorCode::InvalidInput);
}

TEST(PatchNoteServiceTest, AttachesANoteToAVersionOfTheSameGame) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame();
    const auto versionId = fixture.seedVersion(game.id);

    auto command = entry();
    command.versionId = versionId;
    const auto created = drogon::sync_wait(fixture.service.create(publisher(), game.id, command));

    ASSERT_TRUE(created.ok()) << created.error().detail;
    EXPECT_EQ(created.value().gameVersionId, versionId);
}

TEST(PatchNoteServiceTest, RefusesAVersionThatBelongsToAnotherGame) {
    PatchNoteFixture fixture;
    const auto mine = fixture.seedGame();
    const auto other = fixture.seedGame();
    const auto strangerVersion = fixture.seedVersion(other.id);

    auto command = entry();
    command.versionId = strangerVersion;
    const auto created = drogon::sync_wait(fixture.service.create(publisher(), mine.id, command));

    // Checked before the insert, because the foreign key would accept it and the mismatch
    // would only ever show up as a note pointing at somebody else's version.
    ASSERT_FALSE(created.ok());
    EXPECT_EQ(created.error().code, ErrorCode::NotFound);
}

TEST(PatchNoteServiceTest, RefusesToWriteOnSomebodyElsesGame) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame();

    const auto created = drogon::sync_wait(
        fixture.service.create(publisher(launcher::common::randomUuid()), game.id, entry()));

    ASSERT_FALSE(created.ok());
    EXPECT_EQ(created.error().code, ErrorCode::Forbidden);
}

TEST(PatchNoteServiceTest, ADraftGameIsMissingRatherThanForbidden) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame(GameVisibility::Draft);

    const auto created = drogon::sync_wait(
        fixture.service.create(publisher(launcher::common::randomUuid()), game.id, entry()));

    ASSERT_FALSE(created.ok());
    EXPECT_EQ(created.error().code, ErrorCode::NotFound);
}

TEST(PatchNoteServiceTest, LetsAnAdministratorWriteOnAnyGame) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame();

    const auto created =
        drogon::sync_wait(fixture.service.create(administrator(), game.id, entry()));

    ASSERT_TRUE(created.ok()) << created.error().detail;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

TEST(PatchNoteServiceTest, HidesDraftsFromEverybodyButThePublisher) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame();
    ASSERT_TRUE(
        drogon::sync_wait(fixture.service.create(publisher(), game.id, entry("Live", true))).ok());
    ASSERT_TRUE(
        drogon::sync_wait(fixture.service.create(publisher(), game.id, entry("Draft", false)))
            .ok());

    const auto asPlayer = drogon::sync_wait(fixture.service.listForGame(player(), game.id, 20, 0));
    const auto asOwner =
        drogon::sync_wait(fixture.service.listForGame(publisher(), game.id, 20, 0));

    ASSERT_TRUE(asPlayer.ok());
    ASSERT_EQ(asPlayer.value().items.size(), 1u);
    EXPECT_EQ(asPlayer.value().items[0].title, "Live");
    ASSERT_TRUE(asOwner.ok());
    EXPECT_EQ(asOwner.value().items.size(), 2u);
}

TEST(PatchNoteServiceTest, HidesTheDevlogOfADraftGame) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame(GameVisibility::Draft);

    const auto listing = drogon::sync_wait(fixture.service.listForGame(player(), game.id, 20, 0));

    ASSERT_FALSE(listing.ok());
    EXPECT_EQ(listing.error().code, ErrorCode::NotFound);
}

TEST(PatchNoteServiceTest, ClampsAnAbsurdPageSize) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame();
    for (int index = 0; index < 5; ++index) {
        ASSERT_TRUE(
            drogon::sync_wait(fixture.service.create(
                                  publisher(), game.id, entry("Note " + std::to_string(index))))
                .ok());
    }

    const auto listing =
        drogon::sync_wait(fixture.service.listForGame(player(), game.id, 100000, -5));

    ASSERT_TRUE(listing.ok());
    EXPECT_EQ(listing.value().total, 5);
    EXPECT_EQ(listing.value().items.size(), 5u);
}

TEST(PatchNoteServiceTest, RefusesToListForSomebodyWhoCannotBrowseTheCatalog) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame();

    const auto listing =
        drogon::sync_wait(fixture.service.listForGame(Actor{PLAYER, {}}, game.id, 20, 0));

    ASSERT_FALSE(listing.ok());
    EXPECT_EQ(listing.error().code, ErrorCode::Forbidden);
}

// ---------------------------------------------------------------------------
// Editing and removing
// ---------------------------------------------------------------------------

TEST(PatchNoteServiceTest, EditsTheTitleAndTheBody) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame();
    const auto created = drogon::sync_wait(fixture.service.create(publisher(), game.id, entry()));
    ASSERT_TRUE(created.ok());

    PatchNoteUpdate changes;
    changes.title = "What we actually shipped";
    changes.bodyMarkdown = "Corrected.";
    const auto updated =
        drogon::sync_wait(fixture.service.update(publisher(), created.value().id, changes));

    ASSERT_TRUE(updated.ok()) << updated.error().detail;
    EXPECT_EQ(updated.value().title, "What we actually shipped");
    EXPECT_EQ(updated.value().bodyMarkdown, "Corrected.");
}

TEST(PatchNoteServiceTest, PublishesADraftAndTakesItBack) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame();
    const auto created =
        drogon::sync_wait(fixture.service.create(publisher(), game.id, entry("Draft", false)));
    ASSERT_TRUE(created.ok());

    PatchNoteUpdate publish;
    publish.published = true;
    const auto published =
        drogon::sync_wait(fixture.service.update(publisher(), created.value().id, publish));
    ASSERT_TRUE(published.ok()) << published.error().detail;
    EXPECT_TRUE(published.value().published());

    PatchNoteUpdate retract;
    retract.published = false;
    const auto retracted =
        drogon::sync_wait(fixture.service.update(publisher(), created.value().id, retract));

    // Publishing and unpublishing are one field, because a note that went out by mistake has
    // to be able to come back.
    ASSERT_TRUE(retracted.ok()) << retracted.error().detail;
    EXPECT_FALSE(retracted.value().published());
}

TEST(PatchNoteServiceTest, DetachesANoteFromItsVersion) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame();
    auto command = entry();
    command.versionId = fixture.seedVersion(game.id);
    const auto created = drogon::sync_wait(fixture.service.create(publisher(), game.id, command));
    ASSERT_TRUE(created.ok());

    PatchNoteUpdate changes;
    changes.gameVersionId = "";
    const auto updated =
        drogon::sync_wait(fixture.service.update(publisher(), created.value().id, changes));

    ASSERT_TRUE(updated.ok()) << updated.error().detail;
    EXPECT_TRUE(updated.value().gameVersionId.empty());
}

TEST(PatchNoteServiceTest, RefusesToEditANoteOnSomebodyElsesGame) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame();
    const auto created = drogon::sync_wait(fixture.service.create(publisher(), game.id, entry()));
    ASSERT_TRUE(created.ok());

    PatchNoteUpdate changes;
    changes.title = "mine now";
    const auto updated = drogon::sync_wait(fixture.service.update(
        publisher(launcher::common::randomUuid()), created.value().id, changes));

    ASSERT_FALSE(updated.ok());
    EXPECT_EQ(updated.error().code, ErrorCode::Forbidden);
}

TEST(PatchNoteServiceTest, RemovesANote) {
    PatchNoteFixture fixture;
    const auto game = fixture.seedGame();
    const auto created = drogon::sync_wait(fixture.service.create(publisher(), game.id, entry()));
    ASSERT_TRUE(created.ok());

    const auto removed = drogon::sync_wait(fixture.service.remove(publisher(), created.value().id));

    ASSERT_TRUE(removed.ok()) << removed.error().detail;
    EXPECT_TRUE(fixture.notes.notes.empty());
}

TEST(PatchNoteServiceTest, ReportsAnUnknownNoteAsMissing) {
    PatchNoteFixture fixture;

    const auto byUuid =
        drogon::sync_wait(fixture.service.remove(publisher(), launcher::common::randomUuid()));
    const auto byGarbage = drogon::sync_wait(fixture.service.remove(publisher(), "not-a-uuid"));

    ASSERT_FALSE(byUuid.ok());
    EXPECT_EQ(byUuid.error().code, ErrorCode::NotFound);
    ASSERT_FALSE(byGarbage.ok());
    EXPECT_EQ(byGarbage.error().code, ErrorCode::NotFound);
}

} // namespace
