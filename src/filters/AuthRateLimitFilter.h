#pragma once

#include <drogon/HttpFilter.h>

#include <string>

namespace launcher::filters {

/// Per-client-address throttle for the unauthenticated authentication endpoints.
///
/// Attached to login, registration and password-reset requests: without it, those endpoints
/// are an online password-guessing oracle and, because each attempt costs an Argon2id hash,
/// a cheap way to burn the server's CPU.
class AuthRateLimitFilter : public drogon::HttpFilter<AuthRateLimitFilter> {
  public:
    void doFilter(const drogon::HttpRequestPtr& request,
                  drogon::FilterCallback&& reject,
                  drogon::FilterChainCallback&& proceed) override;
};

/// The address a request is attributed to, and therefore the key of every per-address bucket.
///
/// The peer address, unless the peer is one of `server.trustedProxies`, in which case
/// `X-Forwarded-For` decides — see `common::resolveClientAddress` for why it is read from the
/// right and why an unconfigured deployment ignores it outright.
std::string clientAddressOf(const drogon::HttpRequestPtr& request);

} // namespace launcher::filters
