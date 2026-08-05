#include <gtest/gtest.h>

#include <drogon/HttpResponse.h>
#include <json/json.h>

#include "integration/AppHarness.h"

namespace {

using launcher::testing::AppHarness;

AppHarness& harness() {
    return *AppHarness::current();
}

// Readable before anybody has signed in, which is the whole reason it is not behind the auth
// filter: a launcher needs these limits to build its first valid request.
TEST(CapabilitiesEndpointTest, AnswersWithoutAToken) {
    const auto response = harness().get("/api/v1/capabilities");

    ASSERT_NE(response, nullptr);
    EXPECT_EQ(response->statusCode(), drogon::k200OK);

    const auto body = response->getJsonObject();
    ASSERT_NE(body, nullptr);
    EXPECT_EQ((*body)["apiVersion"].asString(), "v1");
}

TEST(CapabilitiesEndpointTest, ReportsTheLimitsThisProcessIsActuallyRunningWith) {
    const auto response = harness().get("/api/v1/capabilities");

    ASSERT_NE(response, nullptr);
    const auto body = response->getJsonObject();
    ASSERT_NE(body, nullptr);

    // The numbers the running configuration was built with, not a repeat of the defaults in
    // the header: the harness boots the app from the development config.
    EXPECT_GT((*body)["uploads"]["maxChunkBytes"].asInt64(), 0);
    EXPECT_GT((*body)["uploads"]["maxBlobBytes"].asInt64(),
              (*body)["uploads"]["maxChunkBytes"].asInt64());
    EXPECT_GT((*body)["media"]["maxBytes"].asInt64(), 0);
    EXPECT_GT((*body)["manifest"]["maxFiles"].asInt64(), 0);
    EXPECT_TRUE((*body)["media"]["contentTypes"].isArray());
}

TEST(CapabilitiesEndpointTest, CarriesARequestIdLikeEveryOtherRoute) {
    const auto response = harness().get("/api/v1/capabilities");

    ASSERT_NE(response, nullptr);
    EXPECT_FALSE(response->getHeader("X-Request-Id").empty());
}

// Cacheable, briefly: a launcher opening several pages must not ask several times, and a
// reconfigured deployment must still take effect promptly.
TEST(CapabilitiesEndpointTest, IsCacheableForAShortWhile) {
    const auto response = harness().get("/api/v1/capabilities");

    ASSERT_NE(response, nullptr);
    EXPECT_NE(response->getHeader("Cache-Control").find("max-age="), std::string::npos);
}

} // namespace
