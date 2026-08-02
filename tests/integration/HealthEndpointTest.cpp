#include <gtest/gtest.h>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpClient.h>
#include <json/json.h>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "app/AppContext.h"
#include "app/Config.h"
#include "app/HttpError.h"
#include "integration/TestDatabase.h"

namespace {

using launcher::app::AppConfig;
using launcher::app::AppContext;
using launcher::testing::TestDatabase;

constexpr const char* TEST_HOST = "127.0.0.1";
constexpr uint16_t TEST_PORT = 18080;

/// Boots the real Drogon application once for the whole suite. Drogon's framework is a
/// process-wide singleton that cannot be restarted, so the server is started in
/// SetUpTestSuite and shut down in TearDownTestSuite.
class HealthEndpointTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        database_ = TestDatabase::createOrNull();

        AppConfig config;
        config.environment = "development";
        AppContext::instance().initialize(
            config, database_ ? database_->client() : drogon::orm::DbClientPtr{});

        launcher::app::registerErrorHandling();
        drogon::app().addListener(TEST_HOST, TEST_PORT);
        drogon::app().setThreadNum(1);

        std::promise<void> started;
        auto startedFuture = started.get_future();
        drogon::app().registerBeginningAdvice([&started]() mutable { started.set_value(); });

        serverThread_ = std::thread([]() { drogon::app().run(); });
        ASSERT_EQ(startedFuture.wait_for(std::chrono::seconds(10)), std::future_status::ready)
            << "the Drogon application did not start in time";
    }

    static void TearDownTestSuite() {
        drogon::app().getLoop()->queueInLoop([]() { drogon::app().quit(); });
        if (serverThread_.joinable()) {
            serverThread_.join();
        }
        AppContext::instance().reset();
        database_.reset();
    }

    static drogon::HttpResponsePtr get(const std::string& path) {
        auto client = drogon::HttpClient::newHttpClient(std::string("http://") + TEST_HOST + ":" +
                                                        std::to_string(TEST_PORT));
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(drogon::Get);
        request->setPath(path);

        auto [result, response] = client->sendRequest(request, 10.0);
        EXPECT_EQ(result, drogon::ReqResult::Ok);
        return response;
    }

    static std::thread serverThread_;
    static std::unique_ptr<TestDatabase> database_;
};

std::thread HealthEndpointTest::serverThread_;
std::unique_ptr<TestDatabase> HealthEndpointTest::database_;

TEST_F(HealthEndpointTest, LivenessReportsOkAndTheBuildVersion) {
    const auto response = get("/api/v1/health");

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
TEST_F(HealthEndpointTest, LivenessDoesNotDependOnTheDatabase) {
    const auto response = get("/api/v1/health");

    ASSERT_NE(response, nullptr);
    EXPECT_EQ(response->statusCode(), drogon::k200OK);
}

TEST_F(HealthEndpointTest, EveryResponseCarriesARequestId) {
    const auto response = get("/api/v1/health");

    ASSERT_NE(response, nullptr);
    EXPECT_FALSE(response->getHeader("X-Request-Id").empty());
}

TEST_F(HealthEndpointTest, ReadinessReflectsDatabaseAvailability) {
    const auto response = get("/api/v1/health/ready");

    ASSERT_NE(response, nullptr);
    const auto body = response->getJsonObject();
    ASSERT_NE(body, nullptr);

    if (database_) {
        EXPECT_EQ(response->statusCode(), drogon::k200OK);
        EXPECT_EQ((*body)["status"].asString(), "ready");
        EXPECT_EQ((*body)["database"].asString(), "up");
    } else {
        EXPECT_EQ(response->statusCode(), drogon::k503ServiceUnavailable);
        EXPECT_EQ((*body)["code"].asString(), "dependency_failure");
    }
}

TEST_F(HealthEndpointTest, UnknownRoutesReturnTheStandardErrorEnvelope) {
    const auto response = get("/api/v1/does-not-exist");

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
