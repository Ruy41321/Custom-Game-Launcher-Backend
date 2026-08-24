#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

#include <functional>
#include <string>

namespace launcher::controllers::v1 {

/// Getting a build onto a machine, and confirming it arrived intact.
///
/// Both routes are POST although neither changes a build. They mint signed URLs and — for the
/// plan — record that a download was handed out, so they are neither cacheable nor free of
/// consequence, which is what GET would promise.
class DownloadController : public drogon::HttpController<DownloadController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(DownloadController::plan,
                  "/api/v1/builds/{1}/download",
                  drogon::Post,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(DownloadController::verify,
                  "/api/v1/builds/{1}/verify",
                  drogon::Post,
                  "launcher::filters::JwtAuthFilter");
    METHOD_LIST_END

    drogon::Task<> plan(drogon::HttpRequestPtr request,
                        std::function<void(const drogon::HttpResponsePtr&)> callback,
                        std::string buildId);

    drogon::Task<> verify(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback,
                          std::string buildId);
};

} // namespace launcher::controllers::v1
