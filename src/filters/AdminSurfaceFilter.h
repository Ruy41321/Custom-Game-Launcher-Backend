#pragma once

#include <drogon/HttpFilter.h>

namespace launcher::filters {

/// Restricts a route to the administrative listener.
///
/// Drogon registers routes on the application, not on a listener, so a controller is reachable
/// on every port the server binds. Without this filter the admin surface would answer on the
/// public :8080 as readily as on :9090, and the second listener would be decoration.
///
/// A request that arrives anywhere else is answered **404, not 403**: from the public listener
/// the administrative surface does not exist. This is the same rule the catalog applies to a
/// draft — a 403 would confirm the route is there and tell an anonymous caller what to attack.
class AdminSurfaceFilter : public drogon::HttpFilter<AdminSurfaceFilter> {
  public:
    void doFilter(const drogon::HttpRequestPtr& request,
                  drogon::FilterCallback&& reject,
                  drogon::FilterChainCallback&& proceed) override;
};

/// Requires the authenticated caller to be an operator — to hold *some* `admin.*` permission.
///
/// Runs after JwtAuthFilter and answers the coarse question only: whether this account may
/// address the administrative surface at all. What it may then do is still decided per action,
/// so an operator holding one admin permission is refused everywhere the others are needed.
///
/// A filter rather than a helper each controller calls, precisely so that no route added later
/// can forget it: the check is part of wiring a route, not of writing its body.
class AdminOperatorFilter : public drogon::HttpFilter<AdminOperatorFilter> {
  public:
    void doFilter(const drogon::HttpRequestPtr& request,
                  drogon::FilterCallback&& reject,
                  drogon::FilterChainCallback&& proceed) override;
};

} // namespace launcher::filters
