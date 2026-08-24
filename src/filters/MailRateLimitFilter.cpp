#include "filters/MailRateLimitFilter.h"

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

void MailRateLimitFilter::doFilter(const drogon::HttpRequestPtr& request,
                                   drogon::FilterCallback&& reject,
                                   drogon::FilterChainCallback&& proceed) {
    const auto& context = app::AppContext::instance();

    if (!context.config().mail.enabled()) {
        reject(app::makeErrorResponse(Error{ErrorCode::NotFound, "no such endpoint"},
                                      app::requestIdOf(request)));
        return;
    }

    auto& limiter = context.mailRateLimiter();
    const auto client = clientAddressOf(request);

    if (limiter.tryAcquire(client)) {
        proceed();
        return;
    }

    const auto retryAfter = limiter.retryAfter(client);
    spdlog::warn("rate limited mail requests from client={}", common::escapeJson(client));

    auto response = app::makeErrorResponse(
        Error{ErrorCode::RateLimited, "too many messages requested; please wait and try again"},
        app::requestIdOf(request));
    response->addHeader("Retry-After", std::to_string(retryAfter.count()));
    reject(response);
}

} // namespace launcher::filters
