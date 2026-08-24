#pragma once

#include <drogon/HttpFilter.h>

namespace launcher::filters {

/// Guards the two routes whose whole job is to put a message in somebody's inbox.
///
/// It does the same two things `CrashRateLimitFilter` does, for the same reasons. A deployment
/// that sends no mail answers **404** rather than a refusal, so a launcher sees a server that
/// does not have the feature instead of one withholding it — and every account on such a
/// deployment can sign in without it, because `validate()` refuses to pair a disabled
/// transport with a required verification.
///
/// The bucket is its own, and not the authentication one. That bucket is tight because every
/// attempt behind it costs an Argon2id hash; these cost no CPU and spend something scarcer —
/// a stranger's inbox, and the deployment's standing with its relay — so the numbers wanted
/// are different, and sharing would let a burst of resend requests lock somebody out of
/// signing in.
class MailRateLimitFilter : public drogon::HttpFilter<MailRateLimitFilter> {
  public:
    void doFilter(const drogon::HttpRequestPtr& request,
                  drogon::FilterCallback&& reject,
                  drogon::FilterChainCallback&& proceed) override;
};

} // namespace launcher::filters
