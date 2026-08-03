#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

#include <functional>
#include <string>

namespace launcher::controllers::v1 {

/// Build-scoped routes: what still needs uploading, where to send it, and the manifest that
/// turns the uploaded blobs into a downloadable build.
///
/// Publishing a build is three steps. Ask which blobs the server is missing; upload each of
/// those through its own resumable session; submit the manifest. Splitting negotiation from
/// transfer is what keeps an update proportional to what actually changed.
class BuildController : public drogon::HttpController<BuildController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(BuildController::missingBlobs,
                  "/api/v1/builds/{1}/blobs/missing",
                  drogon::Post,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(BuildController::beginUpload,
                  "/api/v1/builds/{1}/uploads",
                  drogon::Post,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(BuildController::finalize,
                  "/api/v1/builds/{1}/manifest",
                  drogon::Post,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(BuildController::manifest,
                  "/api/v1/builds/{1}/manifest",
                  drogon::Get,
                  "launcher::filters::JwtAuthFilter");
    METHOD_LIST_END

    drogon::Task<> missingBlobs(drogon::HttpRequestPtr request,
                                std::function<void(const drogon::HttpResponsePtr&)> callback,
                                std::string buildId);

    drogon::Task<> beginUpload(drogon::HttpRequestPtr request,
                               std::function<void(const drogon::HttpResponsePtr&)> callback,
                               std::string buildId);

    drogon::Task<> finalize(drogon::HttpRequestPtr request,
                            std::function<void(const drogon::HttpResponsePtr&)> callback,
                            std::string buildId);

    drogon::Task<> manifest(drogon::HttpRequestPtr request,
                            std::function<void(const drogon::HttpResponsePtr&)> callback,
                            std::string buildId);
};

} // namespace launcher::controllers::v1
