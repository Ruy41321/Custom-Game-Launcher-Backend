#include "controllers/admin/AdminCrashController.h"

#include <json/json.h>

#include <utility>

#include "app/AppContext.h"
#include "common/Error.h"
#include "controllers/admin/AdminJson.h"

namespace launcher::controllers::admin {

drogon::Task<>
AdminCrashController::groups(drogon::HttpRequestPtr request,
                             std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto [limit, offset] = crashPagingOf(request);

    auto page = co_await app::AppContext::instance().crashReportService().listGroups(
        actorOf(request), limit, offset);
    if (!page.ok()) {
        throw common::ApiException(page.error());
    }

    callback(jsonResponse(request, crashGroupPageToJson(page.value(), limit, offset)));
    co_return;
}

drogon::Task<>
AdminCrashController::reports(drogon::HttpRequestPtr request,
                              std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto query = crashQueryOf(request);

    auto page =
        co_await app::AppContext::instance().crashReportService().list(actorOf(request), query);
    if (!page.ok()) {
        throw common::ApiException(page.error());
    }

    callback(jsonResponse(request, crashPageToJson(page.value(), query.limit, query.offset)));
    co_return;
}

drogon::Task<>
AdminCrashController::report(drogon::HttpRequestPtr request,
                             std::function<void(const drogon::HttpResponsePtr&)> callback,
                             std::string reportId) {
    auto found = co_await app::AppContext::instance().crashReportService().find(
        actorOf(request), std::move(reportId));
    if (!found.ok()) {
        throw common::ApiException(found.error());
    }

    callback(jsonResponse(request, crashReportToJson(found.value())));
    co_return;
}

} // namespace launcher::controllers::admin
