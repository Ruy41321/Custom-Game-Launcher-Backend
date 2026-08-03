#include "filters/AuthRateLimitFilter.h"

#include <spdlog/spdlog.h>

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "common/Error.h"
#include "common/Logging.h"

namespace launcher::filters {
namespace {

using common::Error;
using common::ErrorCode;

} // namespace

std::string clientAddressOf(const drogon::HttpRequestPtr& request) {
    return request->getPeerAddr().toIp();
}

void AuthRateLimitFilter::doFilter(const drogon::HttpRequestPtr& request,
                                   drogon::FilterCallback&& reject,
                                   drogon::FilterChainCallback&& proceed) {
    auto& limiter = app::AppContext::instance().authRateLimiter();
    const auto client = clientAddressOf(request);

    if (limiter.tryAcquire(client)) {
        proceed();
        return;
    }

    const auto retryAfter = limiter.retryAfter(client);
    spdlog::warn("rate limited client={} path={}",
                 common::escapeJson(client),
                 common::escapeJson(request->path()));

    auto response = app::makeErrorResponse(
        Error{ErrorCode::RateLimited, "too many attempts; please wait and try again"},
        app::requestIdOf(request));
    response->addHeader("Retry-After", std::to_string(retryAfter.count()));
    reject(response);
}

} // namespace launcher::filters
