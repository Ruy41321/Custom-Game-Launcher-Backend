#pragma once

#include <gtest/gtest.h>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <memory>
#include <string>
#include <thread>

#include "integration/TestDatabase.h"

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

    /// Clears the shared auth throttle so one test's attempts cannot affect the next.
    void resetRateLimiter();

  private:
    drogon::HttpResponsePtr send(const drogon::HttpRequestPtr& request,
                                 const std::string& bearerToken);

    std::unique_ptr<TestDatabase> database_;
    std::thread serverThread_;
    bool started_{false};
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
