#include "integration/AppHarness.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpClient.h>

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

#include "app/AppContext.h"
#include "app/Config.h"
#include "app/HttpError.h"
#include "migrations/MigrationRunner.h"

#ifndef LAUNCHER_MIGRATIONS_DIR
#define LAUNCHER_MIGRATIONS_DIR "migrations"
#endif

namespace launcher::testing {
namespace {

constexpr const char* TEST_HOST = "127.0.0.1";
constexpr uint16_t TEST_PORT = 18080;

AppHarness* HARNESS = nullptr;

app::AppConfig testConfig() {
    app::AppConfig config;
    config.environment = "development";
    config.auth.jwtSecret = "0123456789abcdef0123456789abcdef";
    config.auth.requireVerifiedEmail = true;

    // The cheapest Argon2id parameters libsodium accepts. Cost is what the unit tests cover;
    // here it would only make the suite slow.
    config.auth.argon2OperationsLimit = 1;
    config.auth.argon2MemoryLimitBytes = 8U * 1024U * 1024U;

    // Generous, so ordinary tests are never throttled. The one test that exercises the
    // throttle sets its own limit expectations and resets the bucket afterwards.
    config.rateLimit.authAttempts = 500;
    config.rateLimit.authWindowSeconds = 60;

    return config;
}

} // namespace

AppHarness* AppHarness::current() {
    return HARNESS;
}

AppHarness::AppHarness() {
    HARNESS = this;
}

void AppHarness::SetUp() {
    database_ = TestDatabase::createOrNull();

    if (database_) {
        migrations::MigrationRunner runner(database_->connectionString(), LAUNCHER_MIGRATIONS_DIR);
        const auto migrated = runner.run();
        if (!migrated.ok()) {
            std::cerr << "cannot migrate the integration database: " << migrated.error().detail
                      << '\n';
            database_.reset();
        }
    }

    app::AppContext::instance().initialize(
        testConfig(), database_ ? database_->client() : drogon::orm::DbClientPtr{});

    app::registerErrorHandling();
    drogon::app().addListener(TEST_HOST, TEST_PORT);
    drogon::app().setThreadNum(1);

    std::promise<void> started;
    auto startedFuture = started.get_future();
    drogon::app().registerBeginningAdvice([&started]() { started.set_value(); });

    serverThread_ = std::thread([]() { drogon::app().run(); });
    started_ = startedFuture.wait_for(std::chrono::seconds(15)) == std::future_status::ready;

    if (!started_) {
        throw std::runtime_error("the Drogon application did not start in time");
    }
}

void AppHarness::TearDown() {
    if (started_) {
        drogon::app().getLoop()->queueInLoop([]() { drogon::app().quit(); });
        if (serverThread_.joinable()) {
            serverThread_.join();
        }
    }

    app::AppContext::instance().reset();
    database_.reset();
    HARNESS = nullptr;
}

TestDatabase& AppHarness::database() {
    if (!database_) {
        throw std::logic_error("no test database is configured");
    }
    return *database_;
}

drogon::HttpResponsePtr AppHarness::send(const drogon::HttpRequestPtr& request,
                                         const std::string& bearerToken) {
    if (!bearerToken.empty()) {
        request->addHeader("Authorization", "Bearer " + bearerToken);
    }

    auto client = drogon::HttpClient::newHttpClient(std::string("http://") + TEST_HOST + ":" +
                                                    std::to_string(TEST_PORT));
    auto [result, response] = client->sendRequest(request, 20.0);
    EXPECT_EQ(result, drogon::ReqResult::Ok);
    return response;
}

drogon::HttpResponsePtr AppHarness::get(const std::string& path, const std::string& bearerToken) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->setPath(path);
    return send(request, bearerToken);
}

drogon::HttpResponsePtr AppHarness::postJson(const std::string& path,
                                             const Json::Value& body,
                                             const std::string& bearerToken) {
    auto request = drogon::HttpRequest::newHttpJsonRequest(body);
    request->setMethod(drogon::Post);
    request->setPath(path);
    return send(request, bearerToken);
}

void AppHarness::resetRateLimiter() {
    app::AppContext::instance().authRateLimiter().reset();
}

namespace {

// gtest takes ownership of the environment and deletes it, so it must be heap allocated.
const bool REGISTERED = []() {
    ::testing::AddGlobalTestEnvironment(new AppHarness());
    return true;
}();

} // namespace
} // namespace launcher::testing
