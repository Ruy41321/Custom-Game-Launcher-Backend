#include "filters/JwtAuthFilter.h"

#include <string>

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "common/Error.h"

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
