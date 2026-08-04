#include "controllers/v1/CatalogJson.h"

#include <algorithm>
#include <charconv>

#include "app/AppContext.h"
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

/// A media URL is the configured public base plus the storage key. Composed here rather than
/// in the domain because a base URL is deployment configuration, and for the same reason D22
/// gives about signed links: the stored key survives the deployment moving behind another
/// hostname, and a stored absolute URL would not.
std::string mediaUrlFor(const std::string& storageKey) {
    if (storageKey.empty()) {
        return {};
    }
    auto base = app::AppContext::instance().config().media.publicBaseUrl;
    if (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + "/" + storageKey;
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
    // Empty rather than absent when there is no cover: a client that reads the field either
    // way is one branch simpler than one that has to check the key exists first.
    json["coverUrl"] = mediaUrlFor(game.coverStorageKey);

    Json::Value publisher;
    publisher["id"] = game.publisherUserId;
    publisher["displayName"] = game.publisherDisplayName;
    json["publisher"] = publisher;
    return json;
}

Json::Value mediaToJson(const domain::GameMedia& media) {
    Json::Value json;
    json["id"] = media.id;
    json["gameId"] = media.gameId;
    json["kind"] = domain::toString(media.kind);
    json["url"] = mediaUrlFor(media.storageKey);
    json["contentType"] = media.contentType;
    json["sizeBytes"] = static_cast<Json::Int64>(media.sizeBytes);
    json["altText"] = media.altText;
    json["sortOrder"] = media.sortOrder;
    json["createdAt"] = media.createdAt;
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

    Json::Value media(Json::arrayValue);
    for (const auto& item : detail.media) {
        media.append(mediaToJson(item));
    }
    json["media"] = media;
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
