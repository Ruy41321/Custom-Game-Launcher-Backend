#include "integration/AppHarness.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpClient.h>

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

#include "app/AppContext.h"
#include "app/Bootstrap.h"
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

/// How many times a request that never reached the server is sent again before giving up.
constexpr int TRANSPORT_ATTEMPTS = 3;

/// Stands in for the response a failed request never produced, so a caller that dereferences
/// it reports a failed assertion instead of dying on a null pointer.
drogon::HttpResponsePtr transportFailure() {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k500InternalServerError);
    return response;
}

app::AppConfig testConfig(const std::filesystem::path& blobRoot) {
    app::AppConfig config;
    config.environment = "development";
    config.auth.jwtSecret = "0123456789abcdef0123456789abcdef";
    config.auth.requireVerifiedEmail = true;
    config.storage.blobRoot = blobRoot.string();

    // Small enough that a test can send a "too large" chunk without allocating megabytes, and
    // still large enough for every fixture in the suite.
    config.uploads.maxChunkBytes = 64 * 1024;
    config.uploads.maxBlobBytes = 1024 * 1024;

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

    const auto config = testConfig(blobRoot_.path());
    app::AppContext::instance().initialize(
        config, database_ ? database_->client() : drogon::orm::DbClientPtr{});

    app::registerErrorHandling();
    // The same call production makes: without it Drogon's default one-megabyte body cap would
    // reject upload chunks the deployed server accepts.
    app::configureUploadLimits(config.uploads);
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

    // A transport failure yields no response at all, and every caller here immediately does
    // `response->statusCode()`. Returning the null pointer turned an occasional flake into a
    // segfault with no output, which is how this surfaced in CI: an unreadable crash in a test
    // that had nothing to do with the change under review.
    for (int attempt = 0; attempt < TRANSPORT_ATTEMPTS; ++attempt) {
        auto client = drogon::HttpClient::newHttpClient(std::string("http://") + TEST_HOST + ":" +
                                                        std::to_string(TEST_PORT));
        auto [result, response] = client->sendRequest(request, 20.0);
        if (result == drogon::ReqResult::Ok && response != nullptr) {
            return response;
        }

        // Retried only for BadServerAddress, which means no connection was ever established:
        // the request cannot have reached the server, so sending it again cannot duplicate a
        // registration or an upload chunk. A timeout gets no retry for exactly that reason —
        // it may well have arrived.
        if (result != drogon::ReqResult::BadServerAddress) {
            ADD_FAILURE() << "request to " << request->path() << " failed: " << result;
            return transportFailure();
        }
    }

    ADD_FAILURE() << "request to " << request->path() << " could not reach the test server after "
                  << TRANSPORT_ATTEMPTS << " attempts";
    return transportFailure();
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

drogon::HttpResponsePtr AppHarness::post(const std::string& path, const std::string& bearerToken) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    request->setPath(path);
    return send(request, bearerToken);
}

drogon::HttpResponsePtr AppHarness::patchJson(const std::string& path,
                                              const Json::Value& body,
                                              const std::string& bearerToken) {
    auto request = drogon::HttpRequest::newHttpJsonRequest(body);
    request->setMethod(drogon::Patch);
    request->setPath(path);
    return send(request, bearerToken);
}

drogon::HttpResponsePtr AppHarness::put(const std::string& path, const std::string& bearerToken) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Put);
    request->setPath(path);
    return send(request, bearerToken);
}

drogon::HttpResponsePtr AppHarness::remove(const std::string& path,
                                           const std::string& bearerToken) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Delete);
    request->setPath(path);
    return send(request, bearerToken);
}

drogon::HttpResponsePtr AppHarness::patchBinary(const std::string& path,
                                                const std::string& body,
                                                int64_t uploadOffset,
                                                const std::string& bearerToken) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Patch);
    request->setPath(path);
    request->setContentTypeString("application/offset+octet-stream");
    request->setBody(body);
    if (uploadOffset >= 0) {
        request->addHeader("Upload-Offset", std::to_string(uploadOffset));
    }
    return send(request, bearerToken);
}

Json::Value AppHarness::createVerifiedSession(const std::string& email) {
    Json::Value registration;
    registration["email"] = email;
    registration["password"] = "correct horse battery staple";
    registration["displayName"] = "Test User";

    const auto registered = postJson("/api/v1/auth/register", registration);
    EXPECT_EQ(registered->statusCode(), drogon::k201Created);
    const auto registeredBody = registered->getJsonObject();
    EXPECT_NE(registeredBody, nullptr);

    Json::Value verify;
    verify["token"] = (*registeredBody)["devEmailVerificationToken"].asString();
    EXPECT_EQ(postJson("/api/v1/auth/verify-email", verify)->statusCode(), drogon::k200OK);

    Json::Value credentials;
    credentials["email"] = email;
    credentials["password"] = registration["password"];

    const auto session = postJson("/api/v1/auth/login", credentials);
    EXPECT_EQ(session->statusCode(), drogon::k200OK);
    const auto sessionBody = session->getJsonObject();
    return sessionBody == nullptr ? Json::Value{} : *sessionBody;
}

Json::Value AppHarness::createSessionWithRole(const std::string& email,
                                              const std::string& roleKey) {
    const auto session = createVerifiedSession(email);

    // The devlist is role membership, and the server operator maintains it by hand, so there
    // is no endpoint to call: the grant goes straight into the table.
    database().exec("INSERT INTO user_roles (user_id, role_id) SELECT '" +
                    session["user"]["id"].asString() + "'::uuid, id FROM roles WHERE key = '" +
                    roleKey + "' ON CONFLICT DO NOTHING");

    // The permissions live in the access token, so a token minted before the grant does not
    // carry it; refreshing is what a real client would do.
    Json::Value refresh;
    refresh["refreshToken"] = session["refreshToken"];
    const auto refreshed = postJson("/api/v1/auth/refresh", refresh);
    EXPECT_EQ(refreshed->statusCode(), drogon::k200OK);

    const auto body = refreshed->getJsonObject();
    return body == nullptr ? Json::Value{} : *body;
}

void AppHarness::resetRateLimiter() {
    app::AppContext::instance().authRateLimiter().reset();
}

ScopedAuthRateLimit::ScopedAuthRateLimit(std::size_t attempts, std::chrono::seconds window)
    : previousAttempts_(app::AppContext::instance().config().rateLimit.authAttempts),
      previousWindow_(app::AppContext::instance().config().rateLimit.authWindowSeconds) {
    app::AppContext::instance().authRateLimiter().reconfigure(attempts, window);
}

ScopedAuthRateLimit::~ScopedAuthRateLimit() {
    app::AppContext::instance().authRateLimiter().reconfigure(previousAttempts_, previousWindow_);
}

namespace {

// gtest takes ownership of the environment and deletes it, so it must be heap allocated.
const bool REGISTERED = []() {
    ::testing::AddGlobalTestEnvironment(new AppHarness());
    return true;
}();

} // namespace
} // namespace launcher::testing
