#include "filters/AdminSurfaceFilter.h"

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "common/Error.h"
#include "domain/Admin.h"
#include "filters/JwtAuthFilter.h"

namespace launcher::filters {
namespace {

using common::Error;
using common::ErrorCode;

/// Word for word what the framework's own 404 page says, so a route hidden here is
/// indistinguishable from one that was never registered.
constexpr const char* NO_SUCH_RESOURCE = "The requested resource does not exist.";

} // namespace

void AdminSurfaceFilter::doFilter(const drogon::HttpRequestPtr& request,
                                  drogon::FilterCallback&& reject,
                                  drogon::FilterChainCallback&& proceed) {
    const auto& server = app::AppContext::instance().config().server;

    // The port the connection landed on is the whole gate, and deliberately so: the peer
    // address is *not* checked. The deployed stack binds this listener to 0.0.0.0 inside the
    // container and publishes it as `127.0.0.1:9090:9090`, so the restriction to the host's
    // loopback is the Docker port mapping — and every request arrives from the bridge gateway,
    // never from 127.0.0.1. A peer check would reject exactly the deployment the compose file
    // documents. What stays on the network side stays on the network side.
    if (!server.adminEnabled || request->localAddr().toPort() != server.adminPort) {
        reject(app::makeErrorResponse(Error{ErrorCode::NotFound, NO_SUCH_RESOURCE},
                                      app::requestIdOf(request)));
        return;
    }

    proceed();
}

void AdminOperatorFilter::doFilter(const drogon::HttpRequestPtr& request,
                                   drogon::FilterCallback&& reject,
                                   drogon::FilterChainCallback&& proceed) {
    const auto claims = authenticatedClaims(request);
    if (!claims.has_value()) {
        // Reaching here without claims means the route is missing JwtAuthFilter ahead of this
        // one. That is a wiring bug, and refusing is the only safe reading of it.
        reject(app::makeErrorResponse(
            Error{ErrorCode::Internal, "this route is missing its authentication filter"},
            app::requestIdOf(request)));
        return;
    }

    // Forbidden and not 404 here, unlike the listener gate above: the caller has already
    // proved who they are on a surface only an operator can reach, so there is nothing left to
    // conceal — and an operator who has not been granted the role needs to be told exactly
    // that rather than left staring at a missing page.
    if (!domain::mayUseAdminSurface(domain::Actor{claims->userId, claims->permissions})) {
        reject(app::makeErrorResponse(
            Error{ErrorCode::Forbidden, "this account is not an administrator"},
            app::requestIdOf(request)));
        return;
    }

    proceed();
}

} // namespace launcher::filters
