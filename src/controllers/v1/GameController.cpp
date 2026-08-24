#include "controllers/v1/GameController.h"

#include <json/json.h>

#include <optional>
#include <utility>

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "app/JsonBody.h"
#include "controllers/v1/CatalogJson.h"
#include "domain/ValidationRules.h"
#include "filters/JwtAuthFilter.h"
#include "services/CatalogService.h"

namespace launcher::controllers::v1 {
namespace {

using common::ApiException;
using common::ErrorCode;

[[noreturn]] void fail(const common::Error& error) {
    throw ApiException(error);
}

const services::CatalogService& catalog() {
    return app::AppContext::instance().catalogService();
}

/// Reads an enumerated field, reporting the accepted spellings rather than a bare "invalid".
template<typename T, typename Parser>
T requireEnum(const Json::Value& body, const char* field, Parser parse, const char* accepted) {
    const auto value = app::requireString(body, field);
    const auto parsed = parse(value);
    if (!parsed.has_value()) {
        throw ApiException(ErrorCode::InvalidInput,
                           std::string(field) + " must be one of: " + accepted);
    }
    return *parsed;
}

template<typename T, typename Parser>
T optionalEnum(
    const Json::Value& body, const char* field, Parser parse, const char* accepted, T fallback) {
    if (!body.isMember(field)) {
        return fallback;
    }
    return requireEnum<T>(body, field, parse, accepted);
}

constexpr const char* VISIBILITY_VALUES = "draft, unlisted, public";
constexpr const char* STAGE_VALUES = "demo, alpha, beta, release";
constexpr const char* PLATFORM_VALUES = "windows, linux, macos";
constexpr const char* ARCHITECTURE_VALUES = "x64, arm64";

} // namespace

drogon::Task<>
GameController::explore(drogon::HttpRequestPtr request,
                        std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto query = gameQueryOf(request);

    auto page = co_await catalog().explore(actorOf(request), query);
    if (!page.ok()) {
        fail(page.error());
    }

    callback(jsonResponse(request, gamePageToJson(page.value(), query.limit, query.offset)));
    co_return;
}

drogon::Task<>
GameController::myGames(drogon::HttpRequestPtr request,
                        std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto query = gameQueryOf(request);

    auto page = co_await catalog().publishedByActor(actorOf(request), query);
    if (!page.ok()) {
        fail(page.error());
    }

    callback(jsonResponse(request, gamePageToJson(page.value(), query.limit, query.offset)));
    co_return;
}

drogon::Task<>
GameController::createGame(drogon::HttpRequestPtr request,
                           std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto body = app::requireJsonObject(request);

    services::CreateGameCommand command;
    command.title = app::requireString(body, "title", domain::rules::TITLE_REQUIRED);
    command.slug = app::optionalString(body, "slug");
    command.summary = app::optionalString(body, "summary");
    command.description = app::optionalString(body, "description");
    command.releaseDate = app::optionalString(body, "releaseDate");
    command.visibility = optionalEnum<domain::GameVisibility>(body,
                                                              "visibility",
                                                              domain::parseGameVisibility,
                                                              VISIBILITY_VALUES,
                                                              domain::GameVisibility::Draft);

    auto created = co_await catalog().createGame(actorOf(request), std::move(command));
    if (!created.ok()) {
        fail(created.error());
    }

    callback(jsonResponse(request, gameToJson(created.value()), drogon::k201Created));
    co_return;
}

drogon::Task<>
GameController::gameDetail(drogon::HttpRequestPtr request,
                           std::function<void(const drogon::HttpResponsePtr&)> callback,
                           std::string idOrSlug) {
    auto detail = co_await catalog().gameDetail(actorOf(request), std::move(idOrSlug));
    if (!detail.ok()) {
        fail(detail.error());
    }

    callback(jsonResponse(request, gameDetailToJson(detail.value())));
    co_return;
}

drogon::Task<>
GameController::updateGame(drogon::HttpRequestPtr request,
                           std::function<void(const drogon::HttpResponsePtr&)> callback,
                           std::string idOrSlug) {
    const auto body = app::requireJsonObject(request);

    // Absent means "leave alone", which is why every field goes through isMember rather than
    // through a default: a PATCH that omits the summary must not blank it.
    domain::GameUpdate changes;
    if (body.isMember("title")) {
        changes.title = app::requireString(body, "title", domain::rules::TITLE_REQUIRED);
    }
    if (body.isMember("summary")) {
        changes.summary = app::optionalString(body, "summary");
    }
    if (body.isMember("description")) {
        changes.description = app::optionalString(body, "description");
    }
    if (body.isMember("releaseDate")) {
        changes.releaseDate = app::optionalString(body, "releaseDate");
    }
    if (body.isMember("visibility")) {
        changes.visibility = requireEnum<domain::GameVisibility>(
            body, "visibility", domain::parseGameVisibility, VISIBILITY_VALUES);
    }

    auto updated =
        co_await catalog().updateGame(actorOf(request), std::move(idOrSlug), std::move(changes));
    if (!updated.ok()) {
        fail(updated.error());
    }

    callback(jsonResponse(request, gameToJson(updated.value())));
    co_return;
}

drogon::Task<>
GameController::deleteGame(drogon::HttpRequestPtr request,
                           std::function<void(const drogon::HttpResponsePtr&)> callback,
                           std::string idOrSlug) {
    auto removed = co_await catalog().deleteGame(actorOf(request), std::move(idOrSlug));
    if (!removed.ok()) {
        fail(removed.error());
    }

    callback(noContentResponse(request));
    co_return;
}

drogon::Task<>
GameController::createVersion(drogon::HttpRequestPtr request,
                              std::function<void(const drogon::HttpResponsePtr&)> callback,
                              std::string idOrSlug) {
    const auto body = app::requireJsonObject(request);

    services::CreateVersionCommand command;
    command.semver = app::requireString(body, "semver", domain::rules::VERSION_REQUIRED);
    command.releaseNotes = app::optionalString(body, "releaseNotes");
    command.publish = app::optionalBool(body, "publish");
    command.stage = optionalEnum<domain::BuildStage>(
        body, "stage", domain::parseBuildStage, STAGE_VALUES, domain::BuildStage::Release);

    auto created =
        co_await catalog().createVersion(actorOf(request), std::move(idOrSlug), std::move(command));
    if (!created.ok()) {
        fail(created.error());
    }

    callback(jsonResponse(request, versionToJson(created.value()), drogon::k201Created));
    co_return;
}

drogon::Task<>
GameController::createBuild(drogon::HttpRequestPtr request,
                            std::function<void(const drogon::HttpResponsePtr&)> callback,
                            std::string idOrSlug,
                            std::string versionId) {
    const auto body = app::requireJsonObject(request);

    services::CreateBuildCommand command;
    command.versionId = std::move(versionId);
    command.name = app::optionalString(body, "name");
    command.platform = requireEnum<domain::BuildPlatform>(
        body, "platform", domain::parseBuildPlatform, PLATFORM_VALUES);
    command.architecture = optionalEnum<domain::BuildArchitecture>(body,
                                                                   "architecture",
                                                                   domain::parseBuildArchitecture,
                                                                   ARCHITECTURE_VALUES,
                                                                   domain::BuildArchitecture::X64);

    auto created =
        co_await catalog().createBuild(actorOf(request), std::move(idOrSlug), std::move(command));
    if (!created.ok()) {
        fail(created.error());
    }

    callback(jsonResponse(request, buildToJson(created.value()), drogon::k201Created));
    co_return;
}

drogon::Task<>
GameController::updateVersion(drogon::HttpRequestPtr request,
                              std::function<void(const drogon::HttpResponsePtr&)> callback,
                              std::string idOrSlug,
                              std::string versionId) {
    const auto body = app::requireJsonObject(request);

    // Absent means "leave alone", as it does on a game (see updateGame). `published` is the
    // field this route exists for, and it is read the same way: a PATCH that omits it must not
    // withdraw a version because somebody sent only new release notes.
    domain::GameVersionUpdate changes;
    if (body.isMember("stage")) {
        changes.stage =
            requireEnum<domain::BuildStage>(body, "stage", domain::parseBuildStage, STAGE_VALUES);
    }
    if (body.isMember("releaseNotes")) {
        changes.releaseNotes = app::optionalString(body, "releaseNotes");
    }
    if (body.isMember("published")) {
        if (!body["published"].isBool()) {
            throw ApiException(ErrorCode::InvalidInput, "published must be true or false");
        }
        changes.published = body["published"].asBool();
    }

    auto updated = co_await catalog().updateVersion(
        actorOf(request), std::move(idOrSlug), std::move(versionId), std::move(changes));
    if (!updated.ok()) {
        fail(updated.error());
    }

    callback(jsonResponse(request, versionToJson(updated.value())));
    co_return;
}

drogon::Task<>
GameController::deleteVersion(drogon::HttpRequestPtr request,
                              std::function<void(const drogon::HttpResponsePtr&)> callback,
                              std::string idOrSlug,
                              std::string versionId) {
    auto removed = co_await catalog().deleteVersion(
        actorOf(request), std::move(idOrSlug), std::move(versionId));
    if (!removed.ok()) {
        fail(removed.error());
    }

    callback(noContentResponse(request));
    co_return;
}

} // namespace launcher::controllers::v1
