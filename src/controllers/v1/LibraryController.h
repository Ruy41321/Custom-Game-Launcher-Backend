#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

#include <functional>
#include <string>

namespace launcher::controllers::v1 {

/// The account's server-side library: which games it has added.
///
/// What is *installed* is per machine and stays on the client, so nothing here knows about
/// installations — only membership, which is what has to survive a reinstall.
class LibraryController : public drogon::HttpController<LibraryController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(LibraryController::list,
                  "/api/v1/library",
                  drogon::Get,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(LibraryController::add,
                  "/api/v1/library/{1}",
                  drogon::Put,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(LibraryController::remove,
                  "/api/v1/library/{1}",
                  drogon::Delete,
                  "launcher::filters::JwtAuthFilter");
    METHOD_LIST_END

    drogon::Task<> list(drogon::HttpRequestPtr request,
                        std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<> add(drogon::HttpRequestPtr request,
                       std::function<void(const drogon::HttpResponsePtr&)> callback,
                       std::string idOrSlug);

    drogon::Task<> remove(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback,
                          std::string gameId);
};

} // namespace launcher::controllers::v1
