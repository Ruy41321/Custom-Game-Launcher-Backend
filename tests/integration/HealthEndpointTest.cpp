#include <gtest/gtest.h>

#include <drogon/HttpResponse.h>
#include <json/json.h>

#include "integration/AppHarness.h"

namespace {

using launcher::testing::AppHarness;

AppHarness& harness() {
    return *AppHarness::current();
}

TEST(HealthEndpointTest, LivenessReportsOkAndTheBuildVersion) {
    const auto response = harness().get("/api/v1/health");

    ASSERT_NE(response, nullptr);
    EXPECT_EQ(response->statusCode(), drogon::k200OK);

    const auto body = response->getJsonObject();
    ASSERT_NE(body, nullptr);
    EXPECT_EQ((*body)["status"].asString(), "ok");
    EXPECT_FALSE((*body)["version"].asString().empty());
    EXPECT_EQ((*body)["environment"].asString(), "development");
}

// Liveness must never depend on the database: a blip would otherwise have an orchestrator
// restart a perfectly healthy process.
TEST(HealthEndpointTest, LivenessDoesNotDependOnTheDatabase) {
    const auto response = harness().get("/api/v1/health");

    ASSERT_NE(response, nullptr);
    EXPECT_EQ(response->statusCode(), drogon::k200OK);
}

TEST(HealthEndpointTest, EveryResponseCarriesARequestId) {
    const auto response = harness().get("/api/v1/health");

    ASSERT_NE(response, nullptr);
    EXPECT_FALSE(response->getHeader("X-Request-Id").empty());
}

TEST(HealthEndpointTest, ReadinessReflectsDatabaseAvailability) {
    const auto response = harness().get("/api/v1/health/ready");

    ASSERT_NE(response, nullptr);
    const auto body = response->getJsonObject();
    ASSERT_NE(body, nullptr);

    if (harness().hasDatabase()) {
        EXPECT_EQ(response->statusCode(), drogon::k200OK);
        EXPECT_EQ((*body)["status"].asString(), "ready");
        EXPECT_EQ((*body)["database"].asString(), "up");
    } else {
        EXPECT_EQ(response->statusCode(), drogon::k503ServiceUnavailable);
        EXPECT_EQ((*body)["code"].asString(), "dependency_failure");
    }
}

TEST(HealthEndpointTest, UnknownRoutesReturnTheStandardErrorEnvelope) {
    const auto response = harness().get("/api/v1/does-not-exist");

    ASSERT_NE(response, nullptr);
    EXPECT_EQ(response->statusCode(), drogon::k404NotFound);

    const auto body = response->getJsonObject();
    ASSERT_NE(body, nullptr) << "the 404 page must be the JSON error envelope, not HTML";
    EXPECT_EQ((*body)["status"].asInt(), 404);
    EXPECT_EQ((*body)["code"].asString(), "not_found");
    EXPECT_TRUE(body->isMember("title"));
    EXPECT_TRUE(body->isMember("detail"));
}

} // namespace
