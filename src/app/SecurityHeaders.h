#pragma once

#include <drogon/HttpResponse.h>

#include "app/Config.h"

namespace launcher::app {

/// Adds the headers that describe the deployment to a response that does not already carry
/// them, and leaves the ones it does carry alone.
///
/// "Does not already carry" is load-bearing rather than defensive. The admin console states a
/// `Content-Security-Policy` of its own, far stricter than an API's, and overwriting it here
/// would silently loosen the one surface on this server that a browser actually renders. It is
/// also what keeps this safe to run over Drogon's *cached* 404 response, which is one shared
/// object handed to every request that misses: a second write to it from another event loop is
/// a data race, and skipping the write when the header is already there means the only write
/// that ever happens is the one at construction.
///
/// A pure function of a response and the configuration, so its behaviour is asserted on without
/// an HTTP server.
void applySecurityHeaders(const drogon::HttpResponsePtr& response, const SecurityConfig& config);

/// Installs `applySecurityHeaders` as post-handling advice. Called once at start-up, by the
/// server and by the integration harness alike.
void registerSecurityHeaders(const SecurityConfig& config);

} // namespace launcher::app
