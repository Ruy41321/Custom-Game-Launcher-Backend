#include "controllers/v1/LibraryController.h"

#include <json/json.h>

#include <utility>

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "controllers/v1/CatalogJson.h"
#include "filters/JwtAuthFilter.h"
#include "services/CatalogService.h"

namespace launcher::controllers::v1 {
namespace {

[[noreturn]] void fail(const common::Error& error) {
    throw common::ApiException(error);
}

const services::CatalogService& catalog() {
    return app::AppContext::instance().catalogService();
}

} // namespace

drogon::Task<>
LibraryController::list(drogon::HttpRequestPtr request,
                        std::function<void(const drogon::HttpResponsePtr&)> callback) {
    auto games = co_await catalog().library(actorOf(request));
    if (!games.ok()) {
        fail(games.error());
    }

    Json::Value items(Json::arrayValue);
    for (const auto& game : games.value()) {
        items.append(gameToJson(game));
    }

    Json::Value body;
    body["items"] = items;
    body["total"] = static_cast<Json::Int64>(items.size());

    callback(jsonResponse(request, body));
    co_return;
}

drogon::Task<> LibraryController::add(drogon::HttpRequestPtr request,
                                      std::function<void(const drogon::HttpResponsePtr&)> callback,
                                      std::string idOrSlug) {
    // PUT rather than POST: adding a game you already have is not an error, and the endpoint
    // says so by being idempotent.
    const auto added = co_await catalog().addToLibrary(actorOf(request), std::move(idOrSlug));
    if (!added.ok()) {
        fail(added.error());
    }

    Json::Value body;
    body["status"] = "added";
    callback(jsonResponse(request, body));
    co_return;
}

drogon::Task<>
LibraryController::remove(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback,
                          std::string gameId) {
    const auto removed = co_await catalog().removeFromLibrary(actorOf(request), std::move(gameId));
    if (!removed.ok()) {
        fail(removed.error());
    }

    Json::Value body;
    body["status"] = "removed";
    callback(jsonResponse(request, body));
    co_return;
}

} // namespace launcher::controllers::v1
