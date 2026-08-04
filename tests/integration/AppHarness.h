#pragma once

#include <gtest/gtest.h>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "integration/TestDatabase.h"
#include "support/TemporaryDirectory.h"

namespace launcher::testing {

/// Boots the real Drogon application once for the entire integration binary.
///
/// Drogon's framework is a process-wide singleton that cannot be restarted, so exactly one
/// place may start it. A gtest global environment is that place; individual fixtures share
/// the running server rather than each trying to own it.
///
/// Tests that need a database call `requireDatabase()`, which skips when none is configured.
class AppHarness : public ::testing::Environment {
  public:
    /// The instance gtest owns. Null before the environment is set up.
    static AppHarness* current();

    AppHarness();

    void SetUp() override;

    void TearDown() override;

    bool hasDatabase() const { return database_ != nullptr; }

    TestDatabase& database();

    drogon::HttpResponsePtr get(const std::string& path, const std::string& bearerToken = {});

    drogon::HttpResponsePtr
    postJson(const std::string& path, const Json::Value& body, const std::string& bearerToken = {});

    /// POST with no body at all, which is how a client with nothing to say sends one.
    drogon::HttpResponsePtr post(const std::string& path, const std::string& bearerToken = {});

    drogon::HttpResponsePtr patchJson(const std::string& path,
                                      const Json::Value& body,
                                      const std::string& bearerToken = {});

    drogon::HttpResponsePtr put(const std::string& path, const std::string& bearerToken = {});

    drogon::HttpResponsePtr remove(const std::string& path, const std::string& bearerToken = {});

    /// Raw-body POST with query parameters, as a media upload arrives: the body is the image
    /// itself and everything describing it travels in the query string.
    drogon::HttpResponsePtr postBinary(const std::string& path,
                                       const std::map<std::string, std::string>& parameters,
                                       const std::string& body,
                                       const std::string& bearerToken = {});

    /// Raw-body PATCH, as an upload chunk arrives. `uploadOffset` below zero omits the header
    /// entirely, which is how the "offset is mandatory" rule is exercised.
    drogon::HttpResponsePtr patchBinary(const std::string& path,
                                        const std::string& body,
                                        int64_t uploadOffset,
                                        const std::string& bearerToken = {});

    /// The same requests against the *administrative* listener, which the harness binds on a
    /// second port exactly as a deployment does. Everything reachable there has to be
    /// unreachable on the public port, so both halves of that rule are testable.
    drogon::HttpResponsePtr adminGet(const std::string& path, const std::string& bearerToken = {});

    drogon::HttpResponsePtr adminPostJson(const std::string& path,
                                          const Json::Value& body,
                                          const std::string& bearerToken = {});

    drogon::HttpResponsePtr adminPatchJson(const std::string& path,
                                           const Json::Value& body,
                                           const std::string& bearerToken = {});

    drogon::HttpResponsePtr adminPut(const std::string& path, const std::string& bearerToken = {});

    drogon::HttpResponsePtr adminRemove(const std::string& path,
                                        const std::string& bearerToken = {});

    /// Registers an account, confirms its address and logs in. Returns the session body.
    Json::Value createVerifiedSession(const std::string& email);

    /// Same, plus membership in a role — the manual devlist the server operator maintains.
    Json::Value createSessionWithRole(const std::string& email, const std::string& roleKey);

    /// Where uploaded blobs land for this run. Removed with the harness.
    const std::filesystem::path& blobRoot() const { return blobRoot_.path(); }

    /// Where uploaded artwork lands. A different root from the blobs, exactly as a deployment
    /// keeps them apart — the file server publishes this one without a signature.
    const std::filesystem::path& mediaRoot() const { return mediaRoot_.path(); }

    /// Clears the shared auth throttle so one test's attempts cannot affect the next.
    void resetRateLimiter();

  private:
    drogon::HttpResponsePtr
    send(const drogon::HttpRequestPtr& request, const std::string& bearerToken, uint16_t port);

    drogon::HttpResponsePtr send(const drogon::HttpRequestPtr& request,
                                 const std::string& bearerToken);

    TemporaryDirectory blobRoot_{"launcher-blobs"};
    TemporaryDirectory mediaRoot_{"launcher-media"};
    std::unique_ptr<TestDatabase> database_;
    std::thread serverThread_;
    bool started_{false};
};

/// Narrows the auth throttle for the duration of a test, restoring the harness default
/// afterwards.
///
/// A throttle is a rate, not a count: with the harness default of 500 attempts per minute
/// the bucket refills at 8.3 tokens per second, so whether a fixed number of requests ever
/// empties it depends on how fast the machine answers them. Shrinking the bucket instead
/// makes the assertion hold on any machine.
class ScopedAuthRateLimit {
  public:
    ScopedAuthRateLimit(std::size_t attempts, std::chrono::seconds window);

    ~ScopedAuthRateLimit();

    ScopedAuthRateLimit(const ScopedAuthRateLimit&) = delete;
    ScopedAuthRateLimit& operator=(const ScopedAuthRateLimit&) = delete;
    ScopedAuthRateLimit(ScopedAuthRateLimit&&) = delete;
    ScopedAuthRateLimit& operator=(ScopedAuthRateLimit&&) = delete;

  private:
    std::size_t previousAttempts_;
    std::chrono::seconds previousWindow_;
};

/// Skips the calling test when no database is configured.
#define LAUNCHER_REQUIRE_DATABASE()                                                                \
    do {                                                                                           \
        if (::launcher::testing::AppHarness::current() == nullptr ||                               \
            !::launcher::testing::AppHarness::current()->hasDatabase()) {                          \
            GTEST_SKIP() << "LAUNCHER_TEST_DB_HOST is not set; skipping database integration "     \
                            "test";                                                                \
        }                                                                                          \
    } while (false)

} // namespace launcher::testing
