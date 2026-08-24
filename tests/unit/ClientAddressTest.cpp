#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/ClientAddress.h"

namespace {

using launcher::common::isTrustedProxy;
using launcher::common::resolveClientAddress;

const std::vector<std::string> NO_PROXIES{};

} // namespace

// ---------------------------------------------------------------------------
// Which addresses count as a proxy
// ---------------------------------------------------------------------------

TEST(ClientAddressTest, MatchesAPlainAddressExactly) {
    const std::vector<std::string> trusted{"10.0.0.5"};

    EXPECT_TRUE(isTrustedProxy("10.0.0.5", trusted));
    EXPECT_FALSE(isTrustedProxy("10.0.0.50", trusted));
    EXPECT_FALSE(isTrustedProxy("10.0.0.6", trusted));
}

TEST(ClientAddressTest, MatchesAnIpv4Block) {
    // The form a container bridge needs: the gateway a reverse proxy reaches this server from
    // is assigned by Docker rather than chosen, so naming one address would be a guess.
    const std::vector<std::string> trusted{"172.16.0.0/12"};

    EXPECT_TRUE(isTrustedProxy("172.16.0.1", trusted));
    EXPECT_TRUE(isTrustedProxy("172.18.0.1", trusted));
    EXPECT_TRUE(isTrustedProxy("172.31.255.254", trusted));
    EXPECT_FALSE(isTrustedProxy("172.32.0.1", trusted));
    EXPECT_FALSE(isTrustedProxy("10.0.0.1", trusted));
}

TEST(ClientAddressTest, RefusesWhatIsNotAnAddressRatherThanGuessing) {
    const std::vector<std::string> trusted{"172.16.0.0/12", "not-an-address"};

    EXPECT_FALSE(isTrustedProxy("172.16", trusted));
    EXPECT_FALSE(isTrustedProxy("172.16.0.0.1", trusted));
    EXPECT_FALSE(isTrustedProxy("::1", trusted));
    // A malformed entry matches itself as text and nothing else, which is the harmless
    // outcome: a typo in the configuration trusts nobody rather than everybody.
    EXPECT_TRUE(isTrustedProxy("not-an-address", trusted));
}

TEST(ClientAddressTest, MatchesAnIpv6ProxyAsText) {
    const std::vector<std::string> trusted{"::1"};

    EXPECT_TRUE(isTrustedProxy("::1", trusted));
    EXPECT_FALSE(isTrustedProxy("::2", trusted));
}

// ---------------------------------------------------------------------------
// Which address a request is attributed to
// ---------------------------------------------------------------------------

TEST(ClientAddressTest, IgnoresTheHeaderWhenNoProxyIsConfigured) {
    // The default deployment: clients reach the server directly, so X-Forwarded-For is not a
    // fact about the network but a string somebody typed. Honouring it there would hand every
    // caller a fresh rate-limit bucket per request.
    EXPECT_EQ(resolveClientAddress("203.0.113.9", "198.51.100.1", NO_PROXIES), "203.0.113.9");
}

TEST(ClientAddressTest, IgnoresTheHeaderFromAPeerThatIsNotATrustedProxy) {
    const std::vector<std::string> trusted{"10.0.0.5"};

    EXPECT_EQ(resolveClientAddress("203.0.113.9", "198.51.100.1", trusted), "203.0.113.9");
}

TEST(ClientAddressTest, TakesTheClientFromTheHeaderWhenTheProxyIsTrusted) {
    const std::vector<std::string> trusted{"10.0.0.5"};

    EXPECT_EQ(resolveClientAddress("10.0.0.5", "198.51.100.1", trusted), "198.51.100.1");
}

TEST(ClientAddressTest, ReadsTheHeaderFromTheRight) {
    // The left of this header is whatever the original caller chose to send. Taking the first
    // entry would let anybody claim any address, which is worse than not reading it at all: a
    // per-address throttle whose key the throttled party picks is not a throttle.
    const std::vector<std::string> trusted{"10.0.0.5", "10.0.0.6"};

    EXPECT_EQ(resolveClientAddress("10.0.0.5", "1.2.3.4, 198.51.100.1, 10.0.0.6", trusted),
              "198.51.100.1");
}

TEST(ClientAddressTest, ToleratesTheSpacingRealProxiesWrite) {
    const std::vector<std::string> trusted{"10.0.0.5"};

    EXPECT_EQ(resolveClientAddress("10.0.0.5", "  198.51.100.1  ", trusted), "198.51.100.1");
    EXPECT_EQ(resolveClientAddress("10.0.0.5", "198.51.100.1,", trusted), "198.51.100.1");
}

TEST(ClientAddressTest, FallsBackToThePeerWhenEveryHopIsAProxy) {
    const std::vector<std::string> trusted{"10.0.0.5", "10.0.0.6"};

    EXPECT_EQ(resolveClientAddress("10.0.0.5", "10.0.0.6", trusted), "10.0.0.5");
    EXPECT_EQ(resolveClientAddress("10.0.0.5", "", trusted), "10.0.0.5");
}
