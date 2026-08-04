#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

#include <functional>
#include <string>

namespace launcher::controllers::v1 {

/// Game artwork.
///
/// The upload route takes the image as the raw request body rather than as a multipart form:
/// there is exactly one file and no other field that has to travel with it, and the
/// descriptive parts — the kind, the alt text, the position in the gallery — are query
/// parameters, which keeps the body byte-for-byte the thing that gets hashed and stored.
///
/// Reading is authenticated like the rest of the catalog, but the URLs it hands back are not:
/// artwork is served from a public location, because a cover that needed a signature could
/// never be cached and would expire while somebody was looking at the page.
class MediaController : public drogon::HttpController<MediaController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(MediaController::listForGame,
                  "/api/v1/games/{1}/media",
                  drogon::Get,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(MediaController::upload,
                  "/api/v1/games/{1}/media",
                  drogon::Post,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(MediaController::update,
                  "/api/v1/media/{1}",
                  drogon::Patch,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(MediaController::remove,
                  "/api/v1/media/{1}",
                  drogon::Delete,
                  "launcher::filters::JwtAuthFilter");
    METHOD_LIST_END

    drogon::Task<> listForGame(drogon::HttpRequestPtr request,
                               std::function<void(const drogon::HttpResponsePtr&)> callback,
                               std::string idOrSlug);

    drogon::Task<> upload(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback,
                          std::string idOrSlug);

    drogon::Task<> update(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback,
                          std::string mediaId);

    drogon::Task<> remove(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback,
                          std::string mediaId);
};

} // namespace launcher::controllers::v1
