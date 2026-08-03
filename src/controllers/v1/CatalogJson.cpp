#include "controllers/v1/CatalogJson.h"

#include <algorithm>
#include <charconv>

#include "app/HttpError.h"
#include "filters/JwtAuthFilter.h"

namespace launcher::controllers::v1 {
namespace {

int parseInt(const std::string& text, int fallback) {
    if (text.empty()) {
        return fallback;
    }
    int value = fallback;
    const auto* const end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return fallback;
    }
    return value;
}

} // namespace

domain::Actor actorOf(const drogon::HttpRequestPtr& request) {
    const auto& claims = filters::requireClaims(request);
    return domain::Actor{claims.userId, claims.permissions};
}

drogon::HttpResponsePtr jsonResponse(const drogon::HttpRequestPtr& request,
                                     const Json::Value& body,
                                     drogon::HttpStatusCode status) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(status);
    if (const auto requestId = app::requestIdOf(request); !requestId.empty()) {
        response->addHeader("X-Request-Id", requestId);
    }
    return response;
}

Json::Value gameToJson(const domain::Game& game) {
    Json::Value json;
    json["id"] = game.id;
    json["slug"] = game.slug;
    json["title"] = game.title;
    json["summary"] = game.summary;
    json["description"] = game.description;
    json["releaseDate"] = game.releaseDate;
    json["visibility"] = domain::toString(game.visibility);
    json["createdAt"] = game.createdAt;
    json["updatedAt"] = game.updatedAt;

    Json::Value publisher;
    publisher["id"] = game.publisherUserId;
    publisher["displayName"] = game.publisherDisplayName;
    json["publisher"] = publisher;
    return json;
}

Json::Value versionToJson(const domain::GameVersion& version) {
    Json::Value json;
    json["id"] = version.id;
    json["gameId"] = version.gameId;
    json["semver"] = version.semver;
    json["stage"] = domain::toString(version.stage);
    json["releaseNotes"] = version.releaseNotes;
    json["publishedAt"] = version.publishedAt;
    json["published"] = !version.publishedAt.empty();
    json["createdAt"] = version.createdAt;
    return json;
}

Json::Value buildToJson(const domain::Build& build) {
    Json::Value json;
    json["id"] = build.id;
    json["versionId"] = build.gameVersionId;
    json["platform"] = domain::toString(build.platform);
    json["architecture"] = domain::toString(build.architecture);
    json["status"] = domain::toString(build.status);
    json["manifestSha256"] = build.manifestSha256;
    json["totalSizeBytes"] = static_cast<Json::Int64>(build.totalSizeBytes);
    json["fileCount"] = build.fileCount;
    json["entrypoint"] = build.entrypointRelativePath;
    json["launchArgs"] = build.defaultLaunchArgs;
    json["createdAt"] = build.createdAt;
    json["readyAt"] = build.readyAt;
    return json;
}

Json::Value gameDetailToJson(const domain::GameDetail& detail) {
    Json::Value json;
    json["game"] = gameToJson(detail.game);
    json["inLibrary"] = detail.inLibrary;

    Json::Value versions(Json::arrayValue);
    for (const auto& version : detail.versions) {
        versions.append(versionToJson(version));
    }
    json["versions"] = versions;

    Json::Value builds(Json::arrayValue);
    for (const auto& build : detail.builds) {
        builds.append(buildToJson(build));
    }
    json["builds"] = builds;
    return json;
}

Json::Value gamePageToJson(const repositories::GamePage& page, int limit, int offset) {
    Json::Value items(Json::arrayValue);
    for (const auto& game : page.items) {
        items.append(gameToJson(game));
    }

    Json::Value json;
    json["items"] = items;
    json["total"] = static_cast<Json::Int64>(page.total);
    json["limit"] = limit;
    json["offset"] = offset;
    return json;
}

repositories::GameQuery gameQueryOf(const drogon::HttpRequestPtr& request) {
    repositories::GameQuery query;
    query.search = request->getParameter("search");

    const auto sort = request->getParameter("sort");
    if (sort == "title") {
        query.sort = repositories::GameSort::Title;
    } else if (sort == "recent") {
        query.sort = repositories::GameSort::RecentlyAdded;
    }

    query.limit = std::clamp(
        parseInt(request->getParameter("pageSize"), repositories::DEFAULT_GAME_PAGE_SIZE),
        1,
        repositories::MAX_GAME_PAGE_SIZE);

    // Pages are 1-based on the wire because that is what a UI shows; the repository works in
    // offsets, which is what SQL wants.
    const auto page = std::max(parseInt(request->getParameter("page"), 1), 1);
    query.offset = (page - 1) * query.limit;
    return query;
}

} // namespace launcher::controllers::v1
