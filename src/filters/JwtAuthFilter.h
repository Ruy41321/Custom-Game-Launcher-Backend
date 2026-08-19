#pragma once

#include <drogon/HttpFilter.h>

#include <optional>
#include <string_view>

#include "services/TokenService.h"

namespace launcher::filters {

inline constexpr const char* AUTH_CLAIMS_ATTRIBUTE = "authClaims";

/// The one route a session flagged `passwordChangeRequired` may reach.
///
/// A single allowed path rather than a list of blocked ones, and named here rather than
/// spelled into the filter body so the route and the exemption cannot drift apart. The
/// direction is the point: a route added later is refused to a flagged session by default,
/// which is the same argument the per-account rate limit makes for living in this filter
/// instead of on every route's filter list.
inline constexpr const char* PASSWORD_CHANGE_PATH = "/api/v1/me/password";

/// Requires a valid `Authorization: Bearer <token>` header and publishes the verified claims
/// on the request. Attach it to a route with
/// `ADD_METHOD_TO(..., "launcher::filters::JwtAuthFilter")`.
class JwtAuthFilter : public drogon::HttpFilter<JwtAuthFilter> {
  public:
    void doFilter(const drogon::HttpRequestPtr& request,
                  drogon::FilterCallback&& reject,
                  drogon::FilterChainCallback&& proceed) override;
};

/// Claims published by JwtAuthFilter, or nullopt on an unauthenticated route.
std::optional<services::AccessTokenClaims>
authenticatedClaims(const drogon::HttpRequestPtr& request);

/// Claims for a route that ran through JwtAuthFilter. Throws rather than returning an
/// optional: reaching this on an unauthenticated route is a wiring bug, not a runtime
/// condition to branch on.
const services::AccessTokenClaims& requireClaims(const drogon::HttpRequestPtr& request);

/// Throws ApiException(Forbidden) when the caller lacks the permission.
///
/// The client performs the same check for UX, but that check is advisory; this one is the
/// authority. Every privileged path must call it regardless of what the UI already hid.
void requirePermission(const drogon::HttpRequestPtr& request, std::string_view permission);

} // namespace launcher::filters
