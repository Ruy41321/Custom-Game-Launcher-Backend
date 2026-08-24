#include "filters/CrashRateLimitFilter.h"

#include <spdlog/spdlog.h>

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "common/Error.h"
#include "common/Logging.h"
#include "filters/AuthRateLimitFilter.h"

namespace launcher::filters {
namespace {

using common::Error;
using common::ErrorCode;

} // namespace

void CrashRateLimitFilter::doFilter(const drogon::HttpRequestPtr& request,
                                    drogon::FilterCallback&& reject,
                                    drogon::FilterChainCallback&& proceed) {
    const auto& context = app::AppContext::instance();

    // Turned off means the route does not exist, not that it answers a refusal: a deployment
    // that does not collect crash reports should look to a launcher exactly like one that is
    // too old to have the route, and both are handled by the client giving up quietly.
    if (!context.config().crashReports.enabled) {
        reject(app::makeErrorResponse(Error{ErrorCode::NotFound, "no such endpoint"},
                                      app::requestIdOf(request)));
        return;
    }

    auto& limiter = context.crashRateLimiter();
    const auto client = clientAddressOf(request);

    if (limiter.tryAcquire(client)) {
        proceed();
        return;
    }

    const auto retryAfter = limiter.retryAfter(client);
    spdlog::warn("rate limited crash reports from client={}", common::escapeJson(client));

    auto response = app::makeErrorResponse(
        Error{ErrorCode::RateLimited, "too many crash reports; please wait and try again"},
        app::requestIdOf(request));
    response->addHeader("Retry-After", std::to_string(retryAfter.count()));
    reject(response);
}

} // namespace launcher::filters
