#include "filters/JwtAuthFilter.h"

#include <spdlog/spdlog.h>

#include <string>

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "common/Error.h"
#include "common/Logging.h"

namespace launcher::filters {
namespace {

using common::Error;
using common::ErrorCode;

constexpr std::string_view BEARER_PREFIX = "Bearer ";

std::string extractBearerToken(const drogon::HttpRequestPtr& request) {
    const std::string& header = request->getHeader("Authorization");
    if (header.size() <= BEARER_PREFIX.size() ||
        header.compare(0, BEARER_PREFIX.size(), BEARER_PREFIX) != 0) {
        return {};
    }
    return header.substr(BEARER_PREFIX.size());
}

} // namespace

void JwtAuthFilter::doFilter(const drogon::HttpRequestPtr& request,
                             drogon::FilterCallback&& reject,
                             drogon::FilterChainCallback&& proceed) {
    const auto requestId = app::requestIdOf(request);
    const auto token = extractBearerToken(request);

    if (token.empty()) {
        reject(app::makeErrorResponse(
            Error{ErrorCode::Unauthenticated, "a bearer access token is required"}, requestId));
        return;
    }

    auto claims = app::AppContext::instance().tokenService().verifyAccessToken(token);
    if (!claims.ok()) {
        reject(app::makeErrorResponse(claims.error(), requestId));
        return;
    }

    // The per-account ceiling lives here rather than on each route's filter list, and that is
    // the whole point: every authenticated route on this server runs through this filter, so
    // there is no route present or future that can be given a token-holding caller with no
    // limit at all by somebody forgetting a line. It runs after verification because the key
    // is the account, and an unverified token names nobody.
    auto& limiter = app::AppContext::instance().accountRateLimiter();
    const auto& userId = claims.value().userId;

    if (!limiter.tryAcquire(userId)) {
        const auto retryAfter = limiter.retryAfter(userId);
        spdlog::warn("rate limited account={} path={}",
                     common::escapeJson(userId),
                     common::escapeJson(request->path()));

        auto response = app::makeErrorResponse(
            Error{ErrorCode::RateLimited, "too many requests on this account; please slow down"},
            requestId);
        response->addHeader("Retry-After", std::to_string(retryAfter.count()));
        reject(response);
        return;
    }

    // An account holding a password an operator chose for it reaches exactly one route. The
    // check is here, after verification, for the reason the limiter above is: every
    // authenticated route on this server runs through this filter, so no route can be added
    // later that a flagged session reaches because somebody forgot a line. It costs no
    // database read — the flag rides in the token, and is therefore as stale as the token,
    // which is what changing the password re-issues.
    if (claims.value().passwordChangeRequired && request->path() != PASSWORD_CHANGE_PATH) {
        reject(app::makeErrorResponse(
            Error{ErrorCode::PasswordChangeRequired,
                  "this account is using a temporary password and must choose a new one"},
            requestId));
        return;
    }

    request->attributes()->insert(AUTH_CLAIMS_ATTRIBUTE, std::move(claims).value());
    proceed();
}

std::optional<services::AccessTokenClaims>
authenticatedClaims(const drogon::HttpRequestPtr& request) {
    const auto& attributes = request->attributes();
    if (!attributes->find(AUTH_CLAIMS_ATTRIBUTE)) {
        return std::nullopt;
    }
    return attributes->get<services::AccessTokenClaims>(AUTH_CLAIMS_ATTRIBUTE);
}

const services::AccessTokenClaims& requireClaims(const drogon::HttpRequestPtr& request) {
    const auto& attributes = request->attributes();
    if (!attributes->find(AUTH_CLAIMS_ATTRIBUTE)) {
        throw common::ApiException(ErrorCode::Internal,
                                   "this route is missing its authentication filter");
    }
    return attributes->get<services::AccessTokenClaims>(AUTH_CLAIMS_ATTRIBUTE);
}

void requirePermission(const drogon::HttpRequestPtr& request, std::string_view permission) {
    if (!requireClaims(request).hasPermission(permission)) {
        throw common::ApiException(ErrorCode::Forbidden, "you do not have permission to do that");
    }
}

} // namespace launcher::filters
