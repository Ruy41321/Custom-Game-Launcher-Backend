#include "controllers/v1/HealthController.h"

#include <drogon/orm/Exception.h>
#include <json/json.h>

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "launcher/Version.h"

namespace launcher::controllers::v1 {

namespace {

drogon::HttpResponsePtr jsonResponse(const Json::Value& body, const std::string& requestId) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    if (!requestId.empty()) {
        response->addHeader("X-Request-Id", requestId);
    }
    return response;
}

} // namespace

void HealthController::liveness(const drogon::HttpRequestPtr& request,
                                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    Json::Value body;
    body["status"] = "ok";
    body["version"] = APP_VERSION;
    body["environment"] = app::AppContext::instance().initialized()
                              ? app::AppContext::instance().config().environment
                              : "uninitialized";

    callback(jsonResponse(body, app::requestIdOf(request)));
}

void HealthController::readiness(const drogon::HttpRequestPtr& request,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    const auto requestId = app::requestIdOf(request);
    auto& context = app::AppContext::instance();

    if (!context.initialized() || !context.database()) {
        callback(app::makeErrorResponse(
            {common::ErrorCode::DependencyFailure, "database client is not configured"},
            requestId));
        return;
    }

    context.database()->execSqlAsync(
        "SELECT 1",
        [callback, requestId](const drogon::orm::Result&) {
            Json::Value body;
            body["status"] = "ready";
            body["database"] = "up";
            callback(jsonResponse(body, requestId));
        },
        [callback, requestId](const drogon::orm::DrogonDbException& e) {
            callback(
                app::makeErrorResponse({common::ErrorCode::DependencyFailure,
                                        std::string("database is unreachable: ") + e.base().what()},
                                       requestId));
        });
}

} // namespace launcher::controllers::v1
