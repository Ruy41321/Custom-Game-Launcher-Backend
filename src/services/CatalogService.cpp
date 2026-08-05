#include "services/CatalogService.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

#include "common/Logging.h"
#include "domain/Role.h"
#include "domain/Validation.h"

namespace launcher::services {
namespace {

using common::ErrorCode;
using common::Result;
using common::VoidResult;
using domain::Actor;
using domain::Game;

/// A draft is invisible to everybody but its publisher, and "invisible" means Not Found
/// rather than Forbidden: an unreleased title should not be discoverable by probing ids.
constexpr const char* NO_SUCH_GAME = "no such game";

constexpr const char* NOT_YOURS = "this game belongs to another publisher";

constexpr const char* NO_SUCH_BUILD = "no such build";

constexpr const char* NO_SUCH_VERSION = "no such version";

bool isVisibleTo(const Game& game, const Actor& actor) {
    return domain::mayViewGame(game, actor);
}

bool mayEdit(const Game& game, const Actor& actor) {
    return domain::mayEditGame(game, actor);
}

VoidResult checkLength(std::string_view value, std::size_t limit, const char* field) {
    if (value.size() > limit) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   std::string(field) + " must be at most " +
                                       std::to_string(limit) + " characters");
    }
    return VoidResult::success();
}

VoidResult checkTitle(std::string_view title) {
    if (title.empty()) {
        return VoidResult::failure(ErrorCode::InvalidInput, "title must not be empty");
    }
    return checkLength(title, domain::MAX_TITLE_LENGTH, "title");
}

VoidResult checkOptionalDate(const std::string& date) {
    if (date.empty()) {
        return VoidResult::success();
    }
    return domain::validateReleaseDate(date);
}

repositories::GameQuery clamped(repositories::GameQuery query) {
    query.limit = std::clamp(query.limit, 1, repositories::MAX_GAME_PAGE_SIZE);
    query.offset = std::max(query.offset, 0);
    return query;
}

} // namespace

CatalogService::CatalogService(const repositories::IGameRepository& games,
                               const repositories::IGameVersionRepository& versions,
                               const repositories::IBuildRepository& builds,
                               const repositories::ILibraryRepository& library,
                               const repositories::IMediaRepository& media,
                               MediaReclaimer artwork)
    : games_(games),
      versions_(versions),
      builds_(builds),
      library_(library),
      media_(media),
      artwork_(std::move(artwork)) {}

drogon::Task<Result<Game>> CatalogService::createGame(Actor actor,
                                                      CreateGameCommand command) const {
    if (!actor.can(domain::permissions::GAME_PUBLISH)) {
        co_return Result<Game>::failure(ErrorCode::Forbidden, "you cannot publish games");
    }

    domain::NewGame game;
    game.title = domain::trim(command.title);
    game.summary = domain::trim(command.summary);
    game.description = command.description;
    game.releaseDate = domain::trim(command.releaseDate);
    game.visibility = command.visibility;
    game.publisherUserId = actor.userId;
    game.slug = domain::trim(command.slug);

    if (auto check = checkTitle(game.title); !check.ok()) {
        co_return Result<Game>::failure(check.error());
    }
    if (auto check = checkLength(game.summary, domain::MAX_SUMMARY_LENGTH, "summary");
        !check.ok()) {
        co_return Result<Game>::failure(check.error());
    }
    if (auto check = checkLength(game.description, domain::MAX_DESCRIPTION_LENGTH, "description");
        !check.ok()) {
        co_return Result<Game>::failure(check.error());
    }
    if (auto check = checkOptionalDate(game.releaseDate); !check.ok()) {
        co_return Result<Game>::failure(check.error());
    }

    if (game.slug.empty()) {
        game.slug = domain::slugify(game.title);
    }
    if (auto check = domain::validateSlug(game.slug); !check.ok()) {
        co_return Result<Game>::failure(check.error());
    }

    auto created = co_await games_.create(std::move(game));
    if (created.ok()) {
        spdlog::info("created game id={} publisher={}",
                     common::escapeJson(created.value().id),
                     common::escapeJson(actor.userId));
    }
    co_return created;
}

drogon::Task<Result<Game>>
CatalogService::updateGame(Actor actor, std::string gameId, domain::GameUpdate changes) const {
    auto existing = co_await editableGame(actor, gameId);
    if (!existing.ok()) {
        co_return existing;
    }
    if (changes.empty()) {
        co_return existing;
    }

    if (changes.title.has_value()) {
        changes.title = domain::trim(*changes.title);
        if (auto check = checkTitle(*changes.title); !check.ok()) {
            co_return Result<Game>::failure(check.error());
        }
    }
    if (changes.summary.has_value()) {
        if (auto check = checkLength(*changes.summary, domain::MAX_SUMMARY_LENGTH, "summary");
            !check.ok()) {
            co_return Result<Game>::failure(check.error());
        }
    }
    if (changes.description.has_value()) {
        if (auto check =
                checkLength(*changes.description, domain::MAX_DESCRIPTION_LENGTH, "description");
            !check.ok()) {
            co_return Result<Game>::failure(check.error());
        }
    }
    if (changes.releaseDate.has_value()) {
        changes.releaseDate = domain::trim(*changes.releaseDate);
        if (auto check = checkOptionalDate(*changes.releaseDate); !check.ok()) {
            co_return Result<Game>::failure(check.error());
        }
    }

    auto updated = co_await games_.update(gameId, std::move(changes));
    if (!updated.has_value()) {
        co_return Result<Game>::failure(ErrorCode::NotFound, NO_SUCH_GAME);
    }
    co_return Result<Game>::success(std::move(*updated));
}

drogon::Task<Result<Game>> CatalogService::visibleGame(Actor actor, std::string idOrSlug) const {
    // A slug and a uuid are told apart by shape, so one route serves both without the client
    // having to say which it holds.
    auto game = domain::isUuid(idOrSlug) ? co_await games_.findById(idOrSlug)
                                         : co_await games_.findBySlug(idOrSlug);

    if (!game.has_value() || !isVisibleTo(*game, actor)) {
        co_return Result<Game>::failure(ErrorCode::NotFound, NO_SUCH_GAME);
    }
    co_return Result<Game>::success(std::move(*game));
}

drogon::Task<Result<Game>> CatalogService::editableGame(Actor actor, std::string gameId) const {
    auto game = co_await visibleGame(actor, gameId);
    if (!game.ok()) {
        co_return game;
    }
    if (!mayEdit(game.value(), actor)) {
        co_return Result<Game>::failure(ErrorCode::Forbidden, NOT_YOURS);
    }
    co_return game;
}

drogon::Task<Result<domain::GameDetail>> CatalogService::gameDetail(Actor actor,
                                                                    std::string idOrSlug) const {
    if (!actor.can(domain::permissions::GAME_READ)) {
        co_return Result<domain::GameDetail>::failure(ErrorCode::Forbidden,
                                                      "you cannot browse the catalog");
    }

    auto found = co_await visibleGame(actor, idOrSlug);
    if (!found.ok()) {
        co_return Result<domain::GameDetail>::failure(found.error());
    }

    domain::GameDetail detail;
    detail.game = std::move(found).value();

    // A publisher sees their own unpublished versions; everybody else sees the released ones.
    const bool includeUnpublished = mayEdit(detail.game, actor);
    detail.versions = co_await versions_.listForGame(detail.game.id, includeUnpublished);

    std::vector<std::string> versionIds;
    versionIds.reserve(detail.versions.size());
    for (const auto& version : detail.versions) {
        versionIds.push_back(version.id);
    }
    detail.builds = co_await builds_.listForVersions(std::move(versionIds));
    detail.media = co_await media_.listForGame(detail.game.id);

    detail.inLibrary = co_await library_.contains(actor.userId, detail.game.id);
    co_return Result<domain::GameDetail>::success(std::move(detail));
}

drogon::Task<Result<repositories::GamePage>>
CatalogService::explore(Actor actor, repositories::GameQuery query) const {
    if (!actor.can(domain::permissions::GAME_READ)) {
        co_return Result<repositories::GamePage>::failure(ErrorCode::Forbidden,
                                                          "you cannot browse the catalog");
    }

    // Explore never shows drafts, whoever is asking and whatever the request said.
    query = clamped(std::move(query));
    query.includeUnpublished = false;
    query.publisherUserId.clear();

    co_return Result<repositories::GamePage>::success(co_await games_.search(std::move(query)));
}

drogon::Task<Result<repositories::GamePage>>
CatalogService::publishedByActor(Actor actor, repositories::GameQuery query) const {
    if (!actor.can(domain::permissions::GAME_PUBLISH)) {
        co_return Result<repositories::GamePage>::failure(ErrorCode::Forbidden,
                                                          "you cannot publish games");
    }

    query = clamped(std::move(query));
    query.includeUnpublished = true;
    query.publisherUserId = actor.userId;

    co_return Result<repositories::GamePage>::success(co_await games_.search(std::move(query)));
}

drogon::Task<Result<domain::GameVersion>>
CatalogService::createVersion(Actor actor, std::string gameId, CreateVersionCommand command) const {
    if (!actor.can(domain::permissions::GAME_PUBLISH)) {
        co_return Result<domain::GameVersion>::failure(ErrorCode::Forbidden,
                                                       "you cannot publish games");
    }

    auto game = co_await editableGame(actor, gameId);
    if (!game.ok()) {
        co_return Result<domain::GameVersion>::failure(game.error());
    }

    auto parsed = domain::parseSemver(command.semver);
    if (!parsed.ok()) {
        co_return Result<domain::GameVersion>::failure(parsed.error());
    }
    if (auto check =
            checkLength(command.releaseNotes, domain::MAX_RELEASE_NOTES_LENGTH, "releaseNotes");
        !check.ok()) {
        co_return Result<domain::GameVersion>::failure(check.error());
    }

    domain::NewGameVersion version;
    version.gameId = game.value().id;
    version.version = std::move(parsed).value();
    version.stage = command.stage;
    version.releaseNotes = command.releaseNotes;
    version.publish = command.publish;

    co_return co_await versions_.create(std::move(version));
}

drogon::Task<Result<domain::Build>>
CatalogService::createBuild(Actor actor, std::string gameId, CreateBuildCommand command) const {
    if (!actor.can(domain::permissions::BUILD_UPLOAD)) {
        co_return Result<domain::Build>::failure(ErrorCode::Forbidden, "you cannot upload builds");
    }

    auto game = co_await editableGame(actor, gameId);
    if (!game.ok()) {
        co_return Result<domain::Build>::failure(game.error());
    }

    if (!domain::isUuid(command.versionId)) {
        co_return Result<domain::Build>::failure(ErrorCode::NotFound, "no such version");
    }

    const auto version = co_await versions_.findById(command.versionId);
    // Checking that the version belongs to *this* game stops a caller from hanging a build off
    // somebody else's version by pairing it with a game they do own.
    if (!version.has_value() || version->gameId != game.value().id) {
        co_return Result<domain::Build>::failure(ErrorCode::NotFound, "no such version");
    }

    domain::NewBuild build;
    build.gameVersionId = version->id;
    build.platform = command.platform;
    build.architecture = command.architecture;

    co_return co_await builds_.create(std::move(build));
}

drogon::Task<VoidResult> CatalogService::deleteBuild(Actor actor, std::string buildId) const {
    if (!domain::isUuid(buildId)) {
        co_return VoidResult::failure(ErrorCode::NotFound, NO_SUCH_BUILD);
    }

    const auto ownership = co_await builds_.findOwnership(buildId);
    // The same two questions the upload and download sides ask, from the same place, so this
    // route cannot disagree with them about who may see a draft's builds.
    if (!ownership.has_value() || !domain::mayReadBuild(*ownership, actor)) {
        co_return VoidResult::failure(ErrorCode::NotFound, NO_SUCH_BUILD);
    }
    if (!domain::mayPublishBuild(*ownership, actor)) {
        co_return VoidResult::failure(ErrorCode::Forbidden, NOT_YOURS);
    }

    if (!co_await builds_.remove(buildId)) {
        co_return VoidResult::failure(ErrorCode::NotFound, NO_SUCH_BUILD);
    }
    co_return VoidResult::success();
}

drogon::Task<VoidResult>
CatalogService::deleteVersion(Actor actor, std::string gameId, std::string versionId) const {
    auto game = co_await editableGame(actor, gameId);
    if (!game.ok()) {
        co_return VoidResult::failure(game.error());
    }
    if (!domain::isUuid(versionId)) {
        co_return VoidResult::failure(ErrorCode::NotFound, NO_SUCH_VERSION);
    }

    const auto version = co_await versions_.findById(versionId);
    // A version of another game is reported missing rather than refused: the caller was told
    // about a path that does not exist, not about one they may not use.
    if (!version.has_value() || version->gameId != game.value().id) {
        co_return VoidResult::failure(ErrorCode::NotFound, NO_SUCH_VERSION);
    }

    if (!co_await versions_.remove(versionId)) {
        co_return VoidResult::failure(ErrorCode::NotFound, NO_SUCH_VERSION);
    }
    co_return VoidResult::success();
}

drogon::Task<VoidResult> CatalogService::deleteGame(Actor actor, std::string idOrSlug) const {
    auto game = co_await editableGame(actor, std::move(idOrSlug));
    if (!game.ok()) {
        co_return VoidResult::failure(game.error());
    }

    auto removed = co_await games_.remove(game.value().id);
    // A game that was there a moment ago and is not now was deleted by somebody else holding
    // the same permission. The end state is what was asked for, but reporting it as missing is
    // the honest answer and matches every other delete on this surface.
    if (!removed.has_value()) {
        co_return VoidResult::failure(ErrorCode::NotFound, NO_SUCH_GAME);
    }

    // The rows are gone; the pictures may not be, because two games can share one. The blobs of
    // the builds that went with it are a different question, answered later by the collector.
    co_await artwork_.reclaimAll(std::move(removed->mediaStorageKeys));

    spdlog::info("deleted game id={} by actor={}",
                 common::escapeJson(game.value().id),
                 common::escapeJson(actor.userId));
    co_return VoidResult::success();
}

drogon::Task<VoidResult> CatalogService::addToLibrary(Actor actor, std::string gameId) const {
    if (!actor.can(domain::permissions::LIBRARY_MANAGE)) {
        co_return VoidResult::failure(ErrorCode::Forbidden, "you cannot change your library");
    }

    const auto game = co_await visibleGame(actor, gameId);
    if (!game.ok()) {
        co_return VoidResult::failure(game.error());
    }

    if (!co_await library_.add(actor.userId, game.value().id)) {
        co_return VoidResult::failure(ErrorCode::NotFound, NO_SUCH_GAME);
    }
    co_return VoidResult::success();
}

drogon::Task<VoidResult> CatalogService::removeFromLibrary(Actor actor, std::string gameId) const {
    if (!actor.can(domain::permissions::LIBRARY_MANAGE)) {
        co_return VoidResult::failure(ErrorCode::Forbidden, "you cannot change your library");
    }
    if (!domain::isUuid(gameId)) {
        co_return VoidResult::failure(ErrorCode::NotFound, "that game is not in your library");
    }

    if (!co_await library_.remove(actor.userId, gameId)) {
        co_return VoidResult::failure(ErrorCode::NotFound, "that game is not in your library");
    }
    co_return VoidResult::success();
}

drogon::Task<Result<std::vector<Game>>> CatalogService::library(Actor actor) const {
    if (!actor.can(domain::permissions::LIBRARY_READ)) {
        co_return Result<std::vector<Game>>::failure(ErrorCode::Forbidden,
                                                     "you cannot read your library");
    }
    co_return Result<std::vector<Game>>::success(co_await library_.list(actor.userId));
}

} // namespace launcher::services
