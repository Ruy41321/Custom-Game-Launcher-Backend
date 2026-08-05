#include "controllers/v1/CapabilitiesController.h"

#include <json/json.h>

#include "app/AppContext.h"
#include "app/Capabilities.h"
#include "app/HttpError.h"

namespace launcher::controllers::v1 {

void CapabilitiesController::capabilities(
    const drogon::HttpRequestPtr& request,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto& context = app::AppContext::instance();
    if (!context.initialized()) {
        callback(app::makeErrorResponse(
            {common::ErrorCode::DependencyFailure, "the server is still starting"},
            app::requestIdOf(request)));
        return;
    }

    auto response =
        drogon::HttpResponse::newHttpJsonResponse(app::capabilitiesDocument(context.config()));

    // Limits change when a deployment is reconfigured and restarted, which a client cannot be
    // told about. A minute is short enough that a restart takes effect promptly and long
    // enough that a launcher opening five pages does not ask five times.
    response->addHeader("Cache-Control", "public, max-age=60");
    if (const auto requestId = app::requestIdOf(request); !requestId.empty()) {
        response->addHeader("X-Request-Id", requestId);
    }
    callback(response);
}

} // namespace launcher::controllers::v1
