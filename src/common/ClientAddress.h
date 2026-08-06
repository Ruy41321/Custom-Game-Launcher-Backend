#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace launcher::common {

/// True when `address` is one of the proxies a deployment says it sits behind.
///
/// An entry is either a plain address, compared as text, or an IPv4 CIDR block such as
/// `172.16.0.0/12` — which is the form a container bridge needs, since the gateway a request
/// arrives from is assigned rather than chosen.
bool isTrustedProxy(std::string_view address, const std::vector<std::string>& trustedProxies);

/// Decides which address a request is attributed to.
///
/// Returns `peer` unless the peer is a trusted proxy, in which case `X-Forwarded-For` is walked
/// from the right — the hop nearest this server first — and the first address that is not
/// itself a trusted proxy is the client. Walking from the right is the part that matters: the
/// left of that header is whatever the original caller chose to put there, so an implementation
/// taking the first entry lets anybody claim any address and hand themselves a fresh rate-limit
/// bucket per request.
///
/// With no trusted proxies configured the header is ignored entirely, which is the right answer
/// for a server clients reach directly: there, `X-Forwarded-For` is not a fact about the
/// network, it is a string somebody typed.
std::string resolveClientAddress(std::string peer,
                                 std::string_view forwardedFor,
                                 const std::vector<std::string>& trustedProxies);

} // namespace launcher::common
