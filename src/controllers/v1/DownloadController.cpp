#include "controllers/v1/DownloadController.h"

#include <json/json.h>

#include <utility>
#include <vector>

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "app/JsonBody.h"
#include "controllers/v1/CatalogJson.h"
#include "controllers/v1/DownloadJson.h"
#include "filters/JwtAuthFilter.h"
#include "services/DownloadService.h"

namespace launcher::controllers::v1 {
namespace {

using common::ApiException;
using common::ErrorCode;

[[noreturn]] void fail(const common::Error& error) {
    throw ApiException(error);
}

const services::DownloadService& downloads() {
    return app::AppContext::instance().downloadService();
}

std::vector<services::InstalledFile> readInstalledFiles(const Json::Value& body) {
    if (!body.isMember("files") || !body["files"].isArray()) {
        throw ApiException(ErrorCode::InvalidInput, "files is required and must be an array");
    }

    const auto files = body["files"];
    std::vector<services::InstalledFile> installed;
    installed.reserve(files.size());
    for (const auto& element : files) {
        if (!element.isObject()) {
            throw ApiException(ErrorCode::InvalidInput, "files must contain objects");
        }
        installed.push_back(services::InstalledFile{app::requireString(element, "path"),
                                                    app::requireString(element, "sha256")});
    }
    return installed;
}

} // namespace

drogon::Task<>
DownloadController::plan(drogon::HttpRequestPtr request,
                         std::function<void(const drogon::HttpResponsePtr&)> callback,
                         std::string buildId) {
    // A first install sends no body at all; an update names the build it is coming from.
    const auto body = app::optionalJsonObject(request);

    auto planned = co_await downloads().plan(
        actorOf(request), std::move(buildId), app::optionalString(body, "fromBuildId"));
    if (!planned.ok()) {
        fail(planned.error());
    }

    callback(jsonResponse(request, downloadPlanToJson(planned.value())));
    co_return;
}

drogon::Task<>
DownloadController::verify(drogon::HttpRequestPtr request,
                           std::function<void(const drogon::HttpResponsePtr&)> callback,
                           std::string buildId) {
    const auto body = app::requireJsonObject(request);

    auto report = co_await downloads().verifyInstall(
        actorOf(request), std::move(buildId), readInstalledFiles(body));
    if (!report.ok()) {
        fail(report.error());
    }

    callback(jsonResponse(request, integrityReportToJson(report.value())));
    co_return;
}

} // namespace launcher::controllers::v1
