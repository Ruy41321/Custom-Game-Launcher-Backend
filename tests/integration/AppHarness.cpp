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
#include "app/SecurityHeaders.h"
#include "migrations/MigrationRunner.h"

#ifndef LAUNCHER_MIGRATIONS_DIR
#define LAUNCHER_MIGRATIONS_DIR "migrations"
#endif

namespace launcher::testing {
namespace {

constexpr const char* TEST_HOST = "127.0.0.1";
constexpr uint16_t TEST_PORT = 18080;

/// The administrative listener. Bound for real rather than simulated, because the rule under
/// test — that admin routes answer here and nowhere else — is a property of which socket a
/// request arrived on, and nothing short of a second socket exercises it.
constexpr uint16_t TEST_ADMIN_PORT = 18090;

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

app::AppConfig testConfig(const std::filesystem::path& blobRoot,
                          const std::filesystem::path& mediaRoot) {
    app::AppConfig config;
    config.environment = "development";
    config.auth.jwtSecret = "0123456789abcdef0123456789abcdef";
    config.auth.requireVerifiedEmail = true;
    config.storage.blobRoot = blobRoot.string();
    config.media.root = mediaRoot.string();
    config.media.publicBaseUrl = "http://files.test/media";
    config.server.port = TEST_PORT;
    config.server.adminEnabled = true;
    config.server.adminListenAddress = TEST_HOST;
    config.server.adminPort = TEST_ADMIN_PORT;

    // Small enough that a test can send an oversized image without allocating megabytes.
    config.media.maxBytes = 4096;

    // No grace period here. In a deployment it is what stops the collector eating a build that
    // is still being uploaded, and it has to outlast the slowest publish; in a test it would
    // only mean waiting a day to observe the behaviour under test.
    config.retention.blobGraceSeconds = 0;

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

    // Same reasoning for the mail bucket, whose deployed numbers are three messages in fifteen
    // minutes: a suite that registers dozens of accounts would spend a deployment's allowance
    // in its first fixture.
    config.mail.sendAttempts = 500;
    config.mail.sendWindowSeconds = 60;
    config.mail.linkBaseUrl = "https://launcher.test";

    // Wider still, because this bucket is keyed on the account and the whole suite shares a
    // process: the accounts a fixture reuses would otherwise spend a deployment's allowance
    // between them. The test that exercises it narrows the bucket for its own duration.
    config.rateLimit.accountRequests = 100000;
    config.rateLimit.accountWindowSeconds = 60;

    // On here, off in the struct's own default. A deployment behind TLS wants it and a
    // developer's plain-HTTP machine must not have it, so the header's presence is asserted
    // where a deployment is being simulated and its absence is a unit test on the default.
    config.security.hsts = true;

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

    const auto config = testConfig(blobRoot_.path(), mediaRoot_.path());

    // The one dependency the harness supplies rather than lets the context build: the real one
    // talks SMTP, and there is no mail server in this suite. Borrowed back as a raw pointer so
    // a test can read what was sent — which, now that no response carries a token, is the only
    // way to follow a verification link.
    auto mailSender = std::make_unique<FakeMailSender>();
    mail_ = mailSender.get();

    app::AppContext::instance().initialize(config,
                                           database_ ? database_->client()
                                                     : drogon::orm::DbClientPtr{},
                                           std::move(mailSender));

    app::registerErrorHandling(config.security);
    app::registerSecurityHeaders(config.security);
    // The same call production makes: without it Drogon's default one-megabyte body cap would
    // reject upload chunks the deployed server accepts.
    app::configureBodyLimits(config);
    drogon::app().addListener(TEST_HOST, TEST_PORT);
    drogon::app().addListener(TEST_HOST, TEST_ADMIN_PORT);
    drogon::app().setThreadNum(1);

    std::promise<void> started;
    auto startedFuture = started.get_future();
    drogon::app().registerBeginningAdvice([&started]() { started.set_value(); });

    serverThread_ = std::thread([]() { drogon::app().run(); });
    started_ = startedFuture.wait_for(std::chrono::seconds(15)) == std::future_status::ready;

    if (!started_) {
        throw std::runtime_error("the Drogon application did not start in time");
    }

    // The beginning advice fires when the event loop starts running, which is *not* the same
    // moment the listeners begin accepting. With one listener the gap was small enough to lose
    // in the noise; with two it is wide enough that on a slow machine the first request of a
    // test can beat the accept loop, and all three transport retries fit inside the window. So
    // the harness waits for each listener to answer for real before any test runs.
    awaitListener(TEST_PORT);
    awaitListener(TEST_ADMIN_PORT);
}

void AppHarness::awaitListener(uint16_t port) {
    constexpr int ATTEMPTS = 100;
    constexpr auto PAUSE = std::chrono::milliseconds(50);

    for (int attempt = 0; attempt < ATTEMPTS; ++attempt) {
        auto client = drogon::HttpClient::newHttpClient(std::string("http://") + TEST_HOST + ":" +
                                                        std::to_string(port));
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(drogon::Get);
        request->setPath("/api/v1/health");

        auto [result, response] = client->sendRequest(request, 5.0);
        if (result == drogon::ReqResult::Ok && response != nullptr) {
            return;
        }
        std::this_thread::sleep_for(PAUSE);
    }

    throw std::runtime_error("the listener on port " + std::to_string(port) +
                             " never accepted a connection");
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
    return send(request, bearerToken, TEST_PORT);
}

drogon::HttpResponsePtr AppHarness::send(const drogon::HttpRequestPtr& request,
                                         const std::string& bearerToken,
                                         uint16_t port) {
    if (!bearerToken.empty()) {
        request->addHeader("Authorization", "Bearer " + bearerToken);
    }

    // A transport failure yields no response at all, and every caller here immediately does
    // `response->statusCode()`. Returning the null pointer turned an occasional flake into a
    // segfault with no output, which is how this surfaced in CI: an unreadable crash in a test
    // that had nothing to do with the change under review.
    for (int attempt = 0; attempt < TRANSPORT_ATTEMPTS; ++attempt) {
        auto client = drogon::HttpClient::newHttpClient(std::string("http://") + TEST_HOST + ":" +
                                                        std::to_string(port));
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

drogon::HttpResponsePtr AppHarness::adminGet(const std::string& path,
                                             const std::string& bearerToken) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->setPath(path);
    return send(request, bearerToken, TEST_ADMIN_PORT);
}

drogon::HttpResponsePtr AppHarness::adminPostJson(const std::string& path,
                                                  const Json::Value& body,
                                                  const std::string& bearerToken) {
    auto request = drogon::HttpRequest::newHttpJsonRequest(body);
    request->setMethod(drogon::Post);
    request->setPath(path);
    return send(request, bearerToken, TEST_ADMIN_PORT);
}

drogon::HttpResponsePtr AppHarness::adminPatchJson(const std::string& path,
                                                   const Json::Value& body,
                                                   const std::string& bearerToken) {
    auto request = drogon::HttpRequest::newHttpJsonRequest(body);
    request->setMethod(drogon::Patch);
    request->setPath(path);
    return send(request, bearerToken, TEST_ADMIN_PORT);
}

drogon::HttpResponsePtr AppHarness::adminPut(const std::string& path,
                                             const std::string& bearerToken) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Put);
    request->setPath(path);
    return send(request, bearerToken, TEST_ADMIN_PORT);
}

drogon::HttpResponsePtr AppHarness::adminRemove(const std::string& path,
                                                const std::string& bearerToken) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Delete);
    request->setPath(path);
    return send(request, bearerToken, TEST_ADMIN_PORT);
}

drogon::HttpResponsePtr AppHarness::postBinary(const std::string& path,
                                               const std::map<std::string, std::string>& parameters,
                                               const std::string& body,
                                               const std::string& bearerToken) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    request->setPath(path);
    for (const auto& [name, value] : parameters) {
        request->setParameter(name, value);
    }
    // Deliberately a type the server does not consult: what the body is gets decided by
    // looking at the bytes, and a test that sent image/png would be proving nothing.
    request->setContentTypeString("application/octet-stream");
    request->setBody(body);
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
    EXPECT_EQ(registered->statusCode(), drogon::k201Created) << registered->body();
    const auto registeredBody = registered->getJsonObject();

    // Returning rather than reporting and carrying on. EXPECT_NE does not stop the function,
    // so dereferencing afterwards turned any transport hiccup into a SegFault in a test that
    // had nothing to do with the cause — which is how this presented in CI: an unreadable
    // crash in the quota test, with the real failure three lines above it. A helper that
    // cannot produce a session has to say so and stop.
    if (registeredBody == nullptr) {
        ADD_FAILURE() << "registration produced no session for " << email;
        return {};
    }

    EXPECT_TRUE((*registeredBody)["verificationEmailSent"].asBool())
        << "the fixture cannot verify an address whose message never went out";

    Json::Value verify;
    verify["token"] = tokenMailedTo(email);
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

    // Same reasoning as above: without this the empty session reaches the statement below as a
    // blank uuid literal, and PostgreSQL raises somewhere that looks nothing like the cause.
    const auto userId = session["user"]["id"].asString();
    if (userId.empty()) {
        ADD_FAILURE() << "cannot grant '" << roleKey << "': no session for " << email;
        return {};
    }

    // The grant goes straight into the table. There is an endpoint for it now, on the admin
    // surface, but reaching it would need an operator this helper does not have — and a test
    // fixture that had to bootstrap an administrator to create a publisher would be testing
    // the wrong thing.
    database().exec("INSERT INTO user_roles (user_id, role_id) SELECT '" + userId +
                    "'::uuid, id FROM roles WHERE key = '" + roleKey + "' ON CONFLICT DO NOTHING");

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

FakeMailSender& AppHarness::mail() {
    return *mail_;
}

std::string AppHarness::tokenMailedTo(const std::string& email) {
    return mail_->tokenIn(email);
}

ScopedMailRateLimit::ScopedMailRateLimit(std::size_t attempts, std::chrono::seconds window)
    : previousAttempts_(app::AppContext::instance().config().mail.sendAttempts),
      previousWindow_(app::AppContext::instance().config().mail.sendWindowSeconds) {
    app::AppContext::instance().mailRateLimiter().reconfigure(attempts, window);
}

ScopedMailRateLimit::~ScopedMailRateLimit() {
    app::AppContext::instance().mailRateLimiter().reconfigure(previousAttempts_, previousWindow_);
}

ScopedAuthRateLimit::ScopedAuthRateLimit(std::size_t attempts, std::chrono::seconds window)
    : previousAttempts_(app::AppContext::instance().config().rateLimit.authAttempts),
      previousWindow_(app::AppContext::instance().config().rateLimit.authWindowSeconds) {
    app::AppContext::instance().authRateLimiter().reconfigure(attempts, window);
}

ScopedAuthRateLimit::~ScopedAuthRateLimit() {
    app::AppContext::instance().authRateLimiter().reconfigure(previousAttempts_, previousWindow_);
}

ScopedAccountRateLimit::ScopedAccountRateLimit(std::size_t requests, std::chrono::seconds window)
    : previousRequests_(app::AppContext::instance().config().rateLimit.accountRequests),
      previousWindow_(app::AppContext::instance().config().rateLimit.accountWindowSeconds) {
    app::AppContext::instance().accountRateLimiter().reconfigure(requests, window);
}

ScopedAccountRateLimit::~ScopedAccountRateLimit() {
    app::AppContext::instance().accountRateLimiter().reconfigure(previousRequests_,
                                                                 previousWindow_);
}

namespace {

// gtest takes ownership of the environment and deletes it, so it must be heap allocated.
const bool REGISTERED = []() {
    ::testing::AddGlobalTestEnvironment(new AppHarness());
    return true;
}();

} // namespace
} // namespace launcher::testing
