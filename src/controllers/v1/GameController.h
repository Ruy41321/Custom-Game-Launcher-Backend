#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

#include <functional>
#include <string>

namespace launcher::controllers::v1 {

/// Catalog surface: Explore, game detail, and the publishing routes behind it.
///
/// Every route carries JwtAuthFilter. The launcher is an online client for everything except
/// starting an already installed game, and making the catalog anonymous would mean a second,
/// parallel set of visibility rules to keep correct.
///
/// Handlers are coroutines and take their parameters by value; a reference parameter to a
/// coroutine dangles the moment it first suspends.
class GameController : public drogon::HttpController<GameController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(GameController::explore,
                  "/api/v1/games",
                  drogon::Get,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(GameController::createGame,
                  "/api/v1/games",
                  drogon::Post,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(GameController::myGames,
                  "/api/v1/me/games",
                  drogon::Get,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(GameController::gameDetail,
                  "/api/v1/games/{1}",
                  drogon::Get,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(GameController::updateGame,
                  "/api/v1/games/{1}",
                  drogon::Patch,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(GameController::deleteGame,
                  "/api/v1/games/{1}",
                  drogon::Delete,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(GameController::createVersion,
                  "/api/v1/games/{1}/versions",
                  drogon::Post,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(GameController::createBuild,
                  "/api/v1/games/{1}/versions/{2}/builds",
                  drogon::Post,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(GameController::deleteVersion,
                  "/api/v1/games/{1}/versions/{2}",
                  drogon::Delete,
                  "launcher::filters::JwtAuthFilter");
    METHOD_LIST_END

    drogon::Task<> explore(drogon::HttpRequestPtr request,
                           std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<> createGame(drogon::HttpRequestPtr request,
                              std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<> myGames(drogon::HttpRequestPtr request,
                           std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<> gameDetail(drogon::HttpRequestPtr request,
                              std::function<void(const drogon::HttpResponsePtr&)> callback,
                              std::string idOrSlug);

    drogon::Task<> updateGame(drogon::HttpRequestPtr request,
                              std::function<void(const drogon::HttpResponsePtr&)> callback,
                              std::string idOrSlug);

    drogon::Task<> deleteGame(drogon::HttpRequestPtr request,
                              std::function<void(const drogon::HttpResponsePtr&)> callback,
                              std::string idOrSlug);

    drogon::Task<> createVersion(drogon::HttpRequestPtr request,
                                 std::function<void(const drogon::HttpResponsePtr&)> callback,
                                 std::string idOrSlug);

    drogon::Task<> createBuild(drogon::HttpRequestPtr request,
                               std::function<void(const drogon::HttpResponsePtr&)> callback,
                               std::string idOrSlug,
                               std::string versionId);

    drogon::Task<> deleteVersion(drogon::HttpRequestPtr request,
                                 std::function<void(const drogon::HttpResponsePtr&)> callback,
                                 std::string idOrSlug,
                                 std::string versionId);
};

} // namespace launcher::controllers::v1
