#include <gtest/gtest.h>

#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <atomic>
#include <string>

#include "integration/AppHarness.h"

namespace {

using launcher::testing::AppHarness;

AppHarness& harness() {
    return *AppHarness::current();
}

std::string uniqueEmail(const std::string& prefix) {
    static std::atomic<int> counter{0};
    return prefix + std::to_string(counter.fetch_add(1)) + "@example.test";
}

Json::Value bodyOf(const drogon::HttpResponsePtr& response) {
    EXPECT_NE(response, nullptr);
    const auto json = response->getJsonObject();
    EXPECT_NE(json, nullptr) << "expected a JSON body";
    return json == nullptr ? Json::Value{} : *json;
}

std::string tokenOf(const Json::Value& session) {
    return session["accessToken"].asString();
}

Json::Value anOperator() {
    return harness().createSessionWithRole(uniqueEmail("analytics"), "admin");
}

/// A game with a download event against it, written straight to the table.
///
/// The planner writes these rows for real, and the download tests cover that it does. What
/// this file is about is the *reading*, so the fixture puts known numbers in known places
/// rather than publishing a build to obtain one uncontrolled row.
std::string seedGameWithDownloads(const std::string& publisherId,
                                  const std::string& title,
                                  int fullDownloads,
                                  int deltaDownloads,
                                  int64_t bytesEach) {
    const auto gameId = harness().database().scalar(
        "INSERT INTO games (slug, title, publisher_user_id, visibility) VALUES ('" + title +
        "-slug', '" + title + "', '" + publisherId + "'::uuid, 'public') RETURNING id");

    for (int i = 0; i < fullDownloads; ++i) {
        harness().database().exec(
            "INSERT INTO download_events (game_id, kind, bytes_planned) VALUES ('" + gameId +
            "'::uuid, 'full', " + std::to_string(bytesEach) + ")");
    }
    for (int i = 0; i < deltaDownloads; ++i) {
        harness().database().exec(
            "INSERT INTO download_events (game_id, kind, bytes_planned) VALUES ('" + gameId +
            "'::uuid, 'delta', " + std::to_string(bytesEach) + ")");
    }
    return gameId;
}

} // namespace

TEST(AdminAnalyticsEndpointTest, DoesNotExistOnThePublicListener) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();

    EXPECT_EQ(harness().get("/admin/api/analytics/downloads", tokenOf(session))->statusCode(),
              drogon::k404NotFound);
}

TEST(AdminAnalyticsEndpointTest, RefusesAnOperatorWithoutTheSettingsPermission) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = harness().createSessionWithRole(uniqueEmail("analytics-dev"), "dev");

    EXPECT_EQ(harness().adminGet("/admin/api/analytics/downloads", tokenOf(session))->statusCode(),
              drogon::k403Forbidden);
}

TEST(AdminAnalyticsEndpointTest, CountsDownloadsAndSplitsFullFromDelta) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();
    const auto gameId =
        seedGameWithDownloads(session["user"]["id"].asString(), "analytics-split", 3, 2, 100);

    const auto body =
        bodyOf(harness().adminGet("/admin/api/analytics/downloads", tokenOf(session)));

    // Other tests in this shared database contribute rows of their own, so the totals are
    // asserted as lower bounds and the exact numbers are checked per game below.
    EXPECT_GE(body["totals"]["downloads"].asInt64(), 5);
    EXPECT_GE(body["totals"]["fullDownloads"].asInt64(), 3);
    EXPECT_GE(body["totals"]["deltaDownloads"].asInt64(), 2);
    EXPECT_EQ(body["totals"]["fullDownloads"].asInt64() +
                  body["totals"]["deltaDownloads"].asInt64(),
              body["totals"]["downloads"].asInt64())
        << "every event is one kind or the other";

    bool sawGame = false;
    for (const auto& game : body["topGames"]) {
        if (game["gameId"].asString() != gameId) {
            continue;
        }
        sawGame = true;
        EXPECT_EQ(game["downloads"].asInt64(), 5);
        EXPECT_EQ(game["bytesPlanned"].asInt64(), 500);
        EXPECT_EQ(game["title"].asString(), "analytics-split");
    }
    EXPECT_TRUE(sawGame) << body.toStyledString();
}

TEST(AdminAnalyticsEndpointTest, TheDailySeriesCoversEveryDayInTheWindow) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();

    const auto body =
        bodyOf(harness().adminGet("/admin/api/analytics/downloads?days=7", tokenOf(session)));

    // Eight entries for seven days: the series runs from today minus seven through today
    // inclusive. A chart that skipped the empty days would draw a busier picture than the
    // truth, which is the whole reason generate_series supplies the days.
    EXPECT_EQ(body["days"].asInt(), 7);
    ASSERT_EQ(body["daily"].size(), 8U) << body.toStyledString();
    for (const auto& day : body["daily"]) {
        EXPECT_EQ(day["day"].asString().size(), 10U) << "expected YYYY-MM-DD";
        EXPECT_GE(day["downloads"].asInt64(), 0);
    }
}

TEST(AdminAnalyticsEndpointTest, HonoursTheTopGamesLimit) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();
    const auto publisher = session["user"]["id"].asString();
    seedGameWithDownloads(publisher, "analytics-top-a", 2, 0, 10);
    seedGameWithDownloads(publisher, "analytics-top-b", 1, 0, 10);

    const auto body =
        bodyOf(harness().adminGet("/admin/api/analytics/downloads?topGames=1", tokenOf(session)));

    EXPECT_EQ(body["topGames"].size(), 1U);
}

TEST(AdminAnalyticsEndpointTest, ClampsAWindowNobodyMeantToAskFor) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();

    // Clamped rather than refused: the window comes from a control on a page, and both ways it
    // goes wrong deserve a report. The bound is what stops one request walking a decade.
    const auto absurd =
        bodyOf(harness().adminGet("/admin/api/analytics/downloads?days=100000", tokenOf(session)));
    EXPECT_EQ(absurd["days"].asInt(), 365);

    const auto negative =
        bodyOf(harness().adminGet("/admin/api/analytics/downloads?days=-3", tokenOf(session)));
    EXPECT_EQ(negative["days"].asInt(), 1);
}

TEST(AdminAnalyticsEndpointTest, FallsBackToTheDefaultWindowForNonsense) {
    LAUNCHER_REQUIRE_DATABASE();
    const auto session = anOperator();

    const auto body =
        bodyOf(harness().adminGet("/admin/api/analytics/downloads?days=lots", tokenOf(session)));

    EXPECT_EQ(body["days"].asInt(), 30);
}
