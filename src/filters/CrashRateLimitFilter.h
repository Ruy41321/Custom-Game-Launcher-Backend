#pragma once

#include <drogon/HttpFilter.h>

namespace launcher::filters {

/// Per-client-address throttle for the unauthenticated crash-report route.
///
/// A separate bucket from the authentication one, not a second use of it. The two exist for
/// different reasons and want different numbers: the auth limit is tight because each attempt
/// costs an Argon2id hash and the endpoint is a guessing oracle, while this one is loose
/// because a launcher that crashed five times overnight legitimately sends five reports at
/// once. Sharing a bucket would mean one of them was wrong, and it would let a burst of crash
/// reports lock somebody out of signing in.
class CrashRateLimitFilter : public drogon::HttpFilter<CrashRateLimitFilter> {
  public:
    void doFilter(const drogon::HttpRequestPtr& request,
                  drogon::FilterCallback&& reject,
                  drogon::FilterChainCallback&& proceed) override;
};

} // namespace launcher::filters
