#include "controllers/admin/AdminAnalyticsController.h"

#include <json/json.h>

#include <charconv>
#include <string>
#include <utility>

#include "app/AppContext.h"
#include "common/Error.h"
#include "controllers/admin/AdminJson.h"

namespace launcher::controllers::admin {
namespace {

int parameterAsInt(const drogon::HttpRequestPtr& request, const char* name, int fallback) {
    const std::string text = request->getParameter(name);
    if (text.empty()) {
        return fallback;
    }
    int value = fallback;
    const auto* const end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    return (parsed.ec == std::errc{} && parsed.ptr == end) ? value : fallback;
}

} // namespace

drogon::Task<>
AdminAnalyticsController::downloads(drogon::HttpRequestPtr request,
                                    std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const int days = parameterAsInt(request, "days", repositories::DEFAULT_ANALYTICS_DAYS);
    const int topGames =
        parameterAsInt(request, "topGames", repositories::DEFAULT_ANALYTICS_TOP_GAMES);

    auto result = co_await app::AppContext::instance().analyticsService().downloadReport(
        actorOf(request), days, topGames);
    if (!result.ok()) {
        throw common::ApiException(result.error());
    }

    callback(jsonResponse(request, downloadReportToJson(std::move(result).value())));
    co_return;
}

} // namespace launcher::controllers::admin
