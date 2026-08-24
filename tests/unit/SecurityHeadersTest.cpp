#include <gtest/gtest.h>

#include <drogon/HttpResponse.h>

#include "app/Config.h"
#include "app/SecurityHeaders.h"

namespace {

using launcher::app::applySecurityHeaders;
using launcher::app::SecurityConfig;

drogon::HttpResponsePtr response() {
    return drogon::HttpResponse::newHttpResponse();
}

} // namespace

TEST(SecurityHeadersTest, DescribesAnApiThatIsNeverAPage) {
    auto answer = response();
    applySecurityHeaders(answer, SecurityConfig{});

    EXPECT_EQ(answer->getHeader("X-Content-Type-Options"), "nosniff");
    EXPECT_EQ(answer->getHeader("X-Frame-Options"), "DENY");
    EXPECT_EQ(answer->getHeader("Referrer-Policy"), "no-referrer");
    EXPECT_NE(answer->getHeader("Content-Security-Policy").find("default-src 'none'"),
              std::string::npos);
    EXPECT_NE(answer->getHeader("Content-Security-Policy").find("frame-ancestors 'none'"),
              std::string::npos);
}

TEST(SecurityHeadersTest, SaysNothingAboutTransportSecurityByDefault) {
    // The default matters more than the on switch: an HSTS header on a developer's plain-HTTP
    // machine pins their browser to https://localhost, and there is no way back from that
    // short of clearing browser state.
    auto answer = response();
    applySecurityHeaders(answer, SecurityConfig{});

    EXPECT_TRUE(answer->getHeader("Strict-Transport-Security").empty());
}

TEST(SecurityHeadersTest, StatesTheConfiguredMaxAgeWhenTurnedOn) {
    SecurityConfig config;
    config.hsts = true;
    config.hstsMaxAgeSeconds = 60;

    auto answer = response();
    applySecurityHeaders(answer, config);

    EXPECT_EQ(answer->getHeader("Strict-Transport-Security"), "max-age=60");
}

TEST(SecurityHeadersTest, LeavesAStricterPolicyAloneRatherThanReplacingIt) {
    // The admin console is the one surface here a browser really renders, and it states a far
    // stricter policy of its own. Overwriting it with the API's would silently loosen the only
    // page on this server where a policy does any work.
    auto answer = response();
    answer->addHeader("Content-Security-Policy", "default-src 'none'; connect-src 'self'");
    answer->addHeader("X-Content-Type-Options", "nosniff");

    applySecurityHeaders(answer, SecurityConfig{});

    EXPECT_EQ(answer->getHeader("Content-Security-Policy"),
              "default-src 'none'; connect-src 'self'");
}

TEST(SecurityHeadersTest, IsSafeToRunTwiceOverTheSameResponse) {
    // Drogon caches one not-found response and hands it to every request that misses, so this
    // function has to be idempotent to be usable there at all.
    auto answer = response();
    applySecurityHeaders(answer, SecurityConfig{});
    applySecurityHeaders(answer, SecurityConfig{});

    EXPECT_EQ(answer->getHeader("X-Frame-Options"), "DENY");
}
