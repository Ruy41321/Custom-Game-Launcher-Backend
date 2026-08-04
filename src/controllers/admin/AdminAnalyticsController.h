#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

#include <functional>

namespace launcher::controllers::admin {

/// What this deployment is serving.
///
/// `download_events` has been filled by the download planner since the delta work and read by
/// nothing; this is the surface that reads it.
class AdminAnalyticsController : public drogon::HttpController<AdminAnalyticsController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminAnalyticsController::downloads,
                  "/admin/api/analytics/downloads",
                  drogon::Get,
                  "launcher::filters::AdminSurfaceFilter",
                  "launcher::filters::JwtAuthFilter",
                  "launcher::filters::AdminOperatorFilter");
    METHOD_LIST_END

    drogon::Task<> downloads(drogon::HttpRequestPtr request,
                             std::function<void(const drogon::HttpResponsePtr&)> callback);
};

} // namespace launcher::controllers::admin
