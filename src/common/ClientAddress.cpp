#include "common/ClientAddress.h"

#include <charconv>
#include <cstdint>
#include <optional>

namespace launcher::common {
namespace {

constexpr std::size_t IPV4_OCTETS = 4;
constexpr int IPV4_BITS = 32;

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

std::optional<int> parseNumber(std::string_view text) {
    int value = 0;
    const auto* const end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return std::nullopt;
    }
    return value;
}

/// IPv4 only, and by choice rather than by omission: a CIDR entry exists for the container
/// bridge a reverse proxy reaches this server over, which is IPv4. An IPv6 proxy is written
/// out in full and matched as text.
std::optional<uint32_t> parseIpv4(std::string_view address) {
    uint32_t packed = 0;

    for (std::size_t octet = 0; octet < IPV4_OCTETS; ++octet) {
        const bool last = octet + 1 == IPV4_OCTETS;
        const auto dot = address.find('.');

        // Exactly one separator between octets and none after the last, so `1.2.3.4.5` is
        // refused as firmly as `1.2.3`. Counting only the octets it managed to read accepted
        // the first four of a longer string and ignored the rest.
        if (last != (dot == std::string_view::npos)) {
            return std::nullopt;
        }

        const auto value = parseNumber(address.substr(0, dot));
        if (!value || *value < 0 || *value > 255) {
            return std::nullopt;
        }
        packed = (packed << 8) | static_cast<uint32_t>(*value);

        if (!last) {
            address.remove_prefix(dot + 1);
        }
    }

    return packed;
}

bool matchesEntry(std::string_view address, std::string_view entry) {
    const auto slash = entry.find('/');
    if (slash == std::string_view::npos) {
        return address == entry;
    }

    const auto network = parseIpv4(entry.substr(0, slash));
    const auto prefix = parseNumber(entry.substr(slash + 1));
    const auto candidate = parseIpv4(address);
    if (!network || !candidate || !prefix || *prefix < 0 || *prefix > IPV4_BITS) {
        return false;
    }
    if (*prefix == 0) {
        return true;
    }

    const uint32_t mask = *prefix == IPV4_BITS ? ~0U : ~((1U << (IPV4_BITS - *prefix)) - 1U);
    return (*network & mask) == (*candidate & mask);
}

} // namespace

bool isTrustedProxy(std::string_view address, const std::vector<std::string>& trustedProxies) {
    for (const auto& entry : trustedProxies) {
        if (matchesEntry(address, trim(entry))) {
            return true;
        }
    }
    return false;
}

std::string resolveClientAddress(std::string peer,
                                 std::string_view forwardedFor,
                                 const std::vector<std::string>& trustedProxies) {
    if (trustedProxies.empty() || forwardedFor.empty() || !isTrustedProxy(peer, trustedProxies)) {
        return peer;
    }

    std::size_t end = forwardedFor.size();
    while (end > 0) {
        const auto comma = forwardedFor.rfind(',', end - 1);
        const auto start = comma == std::string_view::npos ? 0 : comma + 1;
        const auto hop = trim(forwardedFor.substr(start, end - start));

        if (!hop.empty() && !isTrustedProxy(hop, trustedProxies)) {
            return std::string(hop);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        end = comma;
    }

    // Every hop named is a proxy we trust, so there is no client in the header to find. The
    // peer is the honest answer rather than the leftmost entry: attributing the request to a
    // proxy throttles the proxy, which is at least true, where trusting an unverifiable string
    // would let one caller wear any address it liked.
    return peer;
}

} // namespace launcher::common
