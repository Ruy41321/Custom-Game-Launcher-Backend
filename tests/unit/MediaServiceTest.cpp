#include <gtest/gtest.h>

#include <drogon/utils/coroutine.h>

#include <string>

#include "common/Random.h"
#include "domain/Role.h"
#include "services/MediaService.h"
#include "support/FakeCatalogRepositories.h"
#include "support/TemporaryDirectory.h"

namespace {

using launcher::common::ErrorCode;
using launcher::domain::Actor;
using launcher::domain::Game;
using launcher::domain::GameVisibility;
using launcher::domain::MediaKind;
using launcher::services::MediaLimits;
using launcher::services::MediaService;
using launcher::services::UploadMediaCommand;
using launcher::storage::MediaStore;
using launcher::testing::FakeGameRepository;
using launcher::testing::FakeMediaRepository;
using launcher::testing::TemporaryDirectory;

namespace permissions = launcher::domain::permissions;

const std::string PUBLISHER = launcher::common::randomUuid();
const std::string PLAYER = launcher::common::randomUuid();

const std::string PNG_HEADER("\x89PNG\r\n\x1a\n", 8);

/// A body that sniffs as a PNG. The suffix makes each one a different content address, which
/// is what the replacement and sharing tests need to tell two pictures apart.
std::string pngBody(const std::string& suffix = "one") {
    return PNG_HEADER + suffix;
}

Actor player(std::string userId = PLAYER) {
    return Actor{std::move(userId),
                 {permissions::LIBRARY_READ, permissions::GAME_READ, permissions::GAME_DOWNLOAD}};
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

/// Keeps the fakes and the store alive for the whole test.
struct MediaFixture {
    MediaFixture()
        : service(games, media, MediaStore{root.path()}, MediaLimits{1024}) {}

    TemporaryDirectory root;
    FakeGameRepository games;
    FakeMediaRepository media;
    MediaService service;

    Game seedGame(GameVisibility visibility = GameVisibility::Public,
                  const std::string& owner = PUBLISHER) {
        Game game;
        game.slug = "seeded-" + launcher::common::randomUuid().substr(0, 8);
        game.title = "Seeded Game";
        game.publisherUserId = owner;
        game.visibility = visibility;
        return games.seed(game);
    }

    bool fileExists(const std::string& storageKey) const {
        return MediaStore{root.path()}.contains(storageKey);
    }
};

UploadMediaCommand cover(std::string bytes = pngBody()) {
    UploadMediaCommand command;
    command.kind = MediaKind::Cover;
    command.bytes = std::move(bytes);
    return command;
}

UploadMediaCommand screenshot(std::string bytes) {
    UploadMediaCommand command;
    command.kind = MediaKind::Screenshot;
    command.bytes = std::move(bytes);
    return command;
}

// ---------------------------------------------------------------------------
// Uploading
// ---------------------------------------------------------------------------

TEST(MediaServiceTest, StoresACoverAndRecordsWhatItIs) {
    MediaFixture fixture;
    const auto game = fixture.seedGame();

    const auto uploaded = drogon::sync_wait(fixture.service.upload(publisher(), game.id, cover()));

    ASSERT_TRUE(uploaded.ok()) << uploaded.error().detail;
    EXPECT_EQ(uploaded.value().kind, MediaKind::Cover);
    EXPECT_EQ(uploaded.value().contentType, "image/png");
    EXPECT_EQ(uploaded.value().sizeBytes, static_cast<int64_t>(pngBody().size()));
    EXPECT_TRUE(fixture.fileExists(uploaded.value().storageKey));
}

TEST(MediaServiceTest, DecidesTheContentTypeFromTheBytesAndNotFromTheRequest) {
    MediaFixture fixture;
    const auto game = fixture.seedGame();

    auto command = cover();
    command.bytes = std::string("\xff\xd8\xff\xe0", 4) + "jpeg body";
    const auto uploaded = drogon::sync_wait(fixture.service.upload(publisher(), game.id, command));

    ASSERT_TRUE(uploaded.ok()) << uploaded.error().detail;
    // The controller never passes a declared type in, and this is why: the value below ends up
    // as the Content-Type of a public URL.
    EXPECT_EQ(uploaded.value().contentType, "image/jpeg");
}

TEST(MediaServiceTest, RefusesABodyThatIsNotAnImage) {
    MediaFixture fixture;
    const auto game = fixture.seedGame();

    auto command = cover();
    command.bytes = "<svg xmlns=\"http://www.w3.org/2000/svg\"><script/></svg>";
    const auto uploaded = drogon::sync_wait(fixture.service.upload(publisher(), game.id, command));

    ASSERT_FALSE(uploaded.ok());
    EXPECT_EQ(uploaded.error().code, ErrorCode::InvalidInput);
}

TEST(MediaServiceTest, RefusesAnEmptyBody) {
    MediaFixture fixture;
    const auto game = fixture.seedGame();

    auto command = cover();
    command.bytes.clear();
    const auto uploaded = drogon::sync_wait(fixture.service.upload(publisher(), game.id, command));

    ASSERT_FALSE(uploaded.ok());
    EXPECT_EQ(uploaded.error().code, ErrorCode::InvalidInput);
}

TEST(MediaServiceTest, RefusesAnImagePastTheSizeLimitBeforeWritingIt) {
    MediaFixture fixture;
    const auto game = fixture.seedGame();

    const auto uploaded = drogon::sync_wait(
        fixture.service.upload(publisher(), game.id, cover(PNG_HEADER + std::string(4096, 'x'))));

    ASSERT_FALSE(uploaded.ok());
    EXPECT_EQ(uploaded.error().code, ErrorCode::InvalidInput);
    EXPECT_TRUE(fixture.media.media.empty());
    // Nothing was written, so there is nothing for a sweep to find later.
    EXPECT_TRUE(std::filesystem::is_empty(fixture.root.path()));
}

TEST(MediaServiceTest, RefusesAnOverlongAltText) {
    MediaFixture fixture;
    const auto game = fixture.seedGame();

    auto command = cover();
    command.altText = std::string(launcher::domain::MAX_ALT_TEXT_LENGTH + 1, 'a');
    const auto uploaded = drogon::sync_wait(fixture.service.upload(publisher(), game.id, command));

    ASSERT_FALSE(uploaded.ok());
    EXPECT_EQ(uploaded.error().code, ErrorCode::InvalidInput);
}

TEST(MediaServiceTest, ASecondCoverReplacesTheFirstAndTakesItsFileWithIt) {
    MediaFixture fixture;
    const auto game = fixture.seedGame();
    const auto first =
        drogon::sync_wait(fixture.service.upload(publisher(), game.id, cover(pngBody("first"))));
    ASSERT_TRUE(first.ok());

    const auto second =
        drogon::sync_wait(fixture.service.upload(publisher(), game.id, cover(pngBody("second"))));

    ASSERT_TRUE(second.ok()) << second.error().detail;
    EXPECT_EQ(fixture.media.media.size(), 1u) << "a game has exactly one cover";
    EXPECT_NE(second.value().storageKey, first.value().storageKey);
    EXPECT_TRUE(fixture.fileExists(second.value().storageKey));
    EXPECT_FALSE(fixture.fileExists(first.value().storageKey))
        << "the displaced picture is no longer referenced and should not survive on disk";
}

TEST(MediaServiceTest, ReUploadingTheSameCoverKeepsItsFile) {
    MediaFixture fixture;
    const auto game = fixture.seedGame();
    const auto first = drogon::sync_wait(fixture.service.upload(publisher(), game.id, cover()));
    ASSERT_TRUE(first.ok());

    const auto again = drogon::sync_wait(fixture.service.upload(publisher(), game.id, cover()));

    ASSERT_TRUE(again.ok()) << again.error().detail;
    EXPECT_EQ(again.value().storageKey, first.value().storageKey);
    // The replaced key is the surviving key here, and deleting it would blank the cover that
    // was just uploaded.
    EXPECT_TRUE(fixture.fileExists(again.value().storageKey));
}

TEST(MediaServiceTest, ScreenshotsAccumulateUpToTheCap) {
    MediaFixture fixture;
    const auto game = fixture.seedGame();

    for (int index = 0; index < launcher::domain::MAX_SCREENSHOTS_PER_GAME; ++index) {
        const auto uploaded = drogon::sync_wait(fixture.service.upload(
            publisher(), game.id, screenshot(pngBody(std::to_string(index)))));
        ASSERT_TRUE(uploaded.ok()) << uploaded.error().detail;
    }

    const auto beyond = drogon::sync_wait(
        fixture.service.upload(publisher(), game.id, screenshot(pngBody("last"))));

    ASSERT_FALSE(beyond.ok());
    EXPECT_EQ(beyond.error().code, ErrorCode::Conflict);
    EXPECT_EQ(fixture.media.media.size(),
              static_cast<std::size_t>(launcher::domain::MAX_SCREENSHOTS_PER_GAME));
}

// ---------------------------------------------------------------------------
// Authorization
// ---------------------------------------------------------------------------

TEST(MediaServiceTest, RefusesToUploadToSomebodyElsesGame) {
    MediaFixture fixture;
    const auto game = fixture.seedGame();

    const auto uploaded = drogon::sync_wait(
        fixture.service.upload(publisher(launcher::common::randomUuid()), game.id, cover()));

    ASSERT_FALSE(uploaded.ok());
    EXPECT_EQ(uploaded.error().code, ErrorCode::Forbidden);
}

TEST(MediaServiceTest, LetsAnAdministratorUploadToAnyGame) {
    MediaFixture fixture;
    const auto game = fixture.seedGame();

    const auto uploaded =
        drogon::sync_wait(fixture.service.upload(administrator(), game.id, cover()));

    ASSERT_TRUE(uploaded.ok()) << uploaded.error().detail;
}

TEST(MediaServiceTest, ADraftBelongingToSomebodyElseIsMissingRatherThanForbidden) {
    MediaFixture fixture;
    const auto game = fixture.seedGame(GameVisibility::Draft);

    const auto uploaded = drogon::sync_wait(
        fixture.service.upload(publisher(launcher::common::randomUuid()), game.id, cover()));

    ASSERT_FALSE(uploaded.ok());
    // Forbidden would confirm the game exists, which is exactly what a draft must not do.
    EXPECT_EQ(uploaded.error().code, ErrorCode::NotFound);
}

TEST(MediaServiceTest, ListsTheArtworkOfAGameTheCallerCanSee) {
    MediaFixture fixture;
    const auto game = fixture.seedGame();
    ASSERT_TRUE(drogon::sync_wait(fixture.service.upload(publisher(), game.id, cover())).ok());

    const auto listing = drogon::sync_wait(fixture.service.listForGame(player(), game.id));

    ASSERT_TRUE(listing.ok()) << listing.error().detail;
    ASSERT_EQ(listing.value().size(), 1u);
    EXPECT_EQ(listing.value()[0].kind, MediaKind::Cover);
}

TEST(MediaServiceTest, HidesTheArtworkOfADraftFromEverybodyButItsPublisher) {
    MediaFixture fixture;
    const auto game = fixture.seedGame(GameVisibility::Draft);

    const auto asPlayer = drogon::sync_wait(fixture.service.listForGame(player(), game.id));
    const auto asOwner = drogon::sync_wait(fixture.service.listForGame(publisher(), game.id));

    ASSERT_FALSE(asPlayer.ok());
    EXPECT_EQ(asPlayer.error().code, ErrorCode::NotFound);
    EXPECT_TRUE(asOwner.ok());
}

// ---------------------------------------------------------------------------
// Editing and removing
// ---------------------------------------------------------------------------

TEST(MediaServiceTest, ChangesTheAltTextAndThePositionButNotThePicture) {
    MediaFixture fixture;
    const auto game = fixture.seedGame();
    const auto uploaded = drogon::sync_wait(fixture.service.upload(publisher(), game.id, cover()));
    ASSERT_TRUE(uploaded.ok());

    launcher::domain::GameMediaUpdate changes;
    changes.altText = "A knight in the rain";
    changes.sortOrder = 3;
    const auto updated =
        drogon::sync_wait(fixture.service.update(publisher(), uploaded.value().id, changes));

    ASSERT_TRUE(updated.ok()) << updated.error().detail;
    EXPECT_EQ(updated.value().altText, "A knight in the rain");
    EXPECT_EQ(updated.value().sortOrder, 3);
    EXPECT_EQ(updated.value().storageKey, uploaded.value().storageKey);
}

TEST(MediaServiceTest, RefusesToEditArtworkOnSomebodyElsesGame) {
    MediaFixture fixture;
    const auto game = fixture.seedGame();
    const auto uploaded = drogon::sync_wait(fixture.service.upload(publisher(), game.id, cover()));
    ASSERT_TRUE(uploaded.ok());

    launcher::domain::GameMediaUpdate changes;
    changes.altText = "mine now";
    const auto updated = drogon::sync_wait(fixture.service.update(
        publisher(launcher::common::randomUuid()), uploaded.value().id, changes));

    ASSERT_FALSE(updated.ok());
    EXPECT_EQ(updated.error().code, ErrorCode::Forbidden);
}

TEST(MediaServiceTest, RemovesTheRowAndTheFileBehindIt) {
    MediaFixture fixture;
    const auto game = fixture.seedGame();
    const auto uploaded = drogon::sync_wait(fixture.service.upload(publisher(), game.id, cover()));
    ASSERT_TRUE(uploaded.ok());

    const auto removed =
        drogon::sync_wait(fixture.service.remove(publisher(), uploaded.value().id));

    ASSERT_TRUE(removed.ok()) << removed.error().detail;
    EXPECT_TRUE(fixture.media.media.empty());
    EXPECT_FALSE(fixture.fileExists(uploaded.value().storageKey));
}

TEST(MediaServiceTest, RemovingOneOfTwoGamesSharingAPictureLeavesTheFileAlone) {
    MediaFixture fixture;
    const auto mine = fixture.seedGame();
    const auto theirs = fixture.seedGame();
    const auto first = drogon::sync_wait(fixture.service.upload(publisher(), mine.id, cover()));
    const auto second = drogon::sync_wait(fixture.service.upload(publisher(), theirs.id, cover()));
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(first.value().storageKey, second.value().storageKey);

    ASSERT_TRUE(drogon::sync_wait(fixture.service.remove(publisher(), first.value().id)).ok());

    // Content addresses are shared, so deleting a row says nothing about whether the picture
    // is still in use. Deleting the file here would blank the other game's cover.
    EXPECT_TRUE(fixture.fileExists(second.value().storageKey));
}

TEST(MediaServiceTest, ReportsAnUnknownImageAsMissing) {
    MediaFixture fixture;

    const auto removed =
        drogon::sync_wait(fixture.service.remove(publisher(), launcher::common::randomUuid()));

    ASSERT_FALSE(removed.ok());
    EXPECT_EQ(removed.error().code, ErrorCode::NotFound);
}

TEST(MediaServiceTest, ReportsAnIdentifierThatIsNotOneAsMissing) {
    MediaFixture fixture;

    const auto removed = drogon::sync_wait(fixture.service.remove(publisher(), "not-a-uuid"));

    ASSERT_FALSE(removed.ok());
    EXPECT_EQ(removed.error().code, ErrorCode::NotFound);
}

} // namespace
