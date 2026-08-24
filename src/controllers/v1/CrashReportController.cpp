#include "controllers/v1/CrashReportController.h"

#include <json/json.h>

#include <utility>

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "app/JsonBody.h"
#include "controllers/v1/CatalogJson.h"
#include "filters/CrashRateLimitFilter.h"
#include "services/CrashReportService.h"

namespace launcher::controllers::v1 {

drogon::Task<>
CrashReportController::submit(drogon::HttpRequestPtr request,
                              std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto body = app::requireJsonObject(request);

    domain::NewCrashReport report;
    report.kind = app::requireString(body, "kind");
    report.occurredAt = app::requireString(body, "occurredAt");
    report.launcherVersion = app::optionalString(body, "launcherVersion");
    report.platform = app::optionalString(body, "platform");
    report.exceptionType = app::optionalString(body, "exceptionType");
    report.message = app::optionalString(body, "message");
    report.stackTrace = app::optionalString(body, "stackTrace");

    const auto stored =
        co_await app::AppContext::instance().crashReportService().submit(std::move(report));
    if (!stored.ok()) {
        throw common::ApiException(stored.error());
    }

    // The fingerprint and nothing else. A client has no use for the row it just created, and
    // handing back what it sent would make this route an echo an anonymous caller can use to
    // check what the server stores. The fingerprint is worth returning because it is the one
    // thing the client did not know: a person filing a bug can quote it.
    Json::Value response;
    response["fingerprint"] = stored.value().fingerprint;
    callback(jsonResponse(request, response, drogon::k202Accepted));
    co_return;
}

} // namespace launcher::controllers::v1
