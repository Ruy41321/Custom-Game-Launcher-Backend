#include <gtest/gtest.h>

#include <drogon/utils/coroutine.h>

#include <string>

#include "common/Random.h"
#include "domain/Role.h"
#include "services/CrashReportService.h"
#include "support/FakeCrashReportRepository.h"

namespace {

using launcher::common::ErrorCode;
using launcher::domain::Actor;
using launcher::domain::NewCrashReport;
using launcher::services::CrashReportService;
using launcher::services::CrashReportSettings;
using launcher::testing::FakeCrashReportRepository;

namespace permissions = launcher::domain::permissions;

Actor anonymousPlayer() {
    return Actor{launcher::common::randomUuid(), {permissions::GAME_READ}};
}

Actor operatorWhoReadsCrashes() {
    return Actor{launcher::common::randomUuid(), {permissions::ADMIN_CRASHES_READ}};
}

NewCrashReport crash(std::string exceptionType = "System.IO.IOException",
                     std::string stack = "   at Fetcher.FetchAsync() in Fetcher.cs:line 42") {
    NewCrashReport report;
    report.kind = "unhandled";
    report.occurredAt = "2026-08-05T12:00:00Z";
    report.launcherVersion = "0.1.0";
    report.exceptionType = std::move(exceptionType);
    report.message = "something broke";
    report.stackTrace = std::move(stack);
    return report;
}

struct CrashFixture {
    CrashFixture()
        : service(reports, CrashReportSettings{}) {}

    FakeCrashReportRepository reports;
    CrashReportService service;
};

// ---------------------------------------------------------------------------
// Submitting — no account, and none recorded
// ---------------------------------------------------------------------------

TEST(CrashReportServiceTest, StoresAReportWithoutAnybodySigningIn) {
    CrashFixture fixture;

    const auto stored = drogon::sync_wait(fixture.service.submit(crash()));

    ASSERT_TRUE(stored.ok()) << stored.error().detail;
    EXPECT_EQ(fixture.reports.reports.size(), 1U);
    EXPECT_EQ(stored.value().exceptionType, "System.IO.IOException");
}

// The fingerprint is the server's to compute: a client that chose its own could hide a crash
// among a thousand distinct ones, and two client versions could disagree about what one bug is.
TEST(CrashReportServiceTest, TheServerComputesTheFingerprintAndTheReportCarriesIt) {
    CrashFixture fixture;

    const auto stored = drogon::sync_wait(fixture.service.submit(crash()));

    ASSERT_TRUE(stored.ok());
    EXPECT_EQ(stored.value().fingerprint, launcher::domain::crashFingerprint(crash()));
    EXPECT_EQ(stored.value().fingerprint.size(), 64U);
}

TEST(CrashReportServiceTest, RefusesAMalformedReportBeforeStoringAnything) {
    CrashFixture fixture;

    auto malformed = crash();
    malformed.occurredAt = "not a timestamp";
    const auto stored = drogon::sync_wait(fixture.service.submit(malformed));

    ASSERT_FALSE(stored.ok());
    EXPECT_EQ(stored.error().code, ErrorCode::InvalidInput);
    EXPECT_TRUE(fixture.reports.reports.empty());
}

TEST(CrashReportServiceTest, TrimsWhatTheClientPadded) {
    CrashFixture fixture;

    auto padded = crash();
    padded.kind = "  unhandled  ";
    ASSERT_TRUE(drogon::sync_wait(fixture.service.submit(padded)).ok());

    EXPECT_EQ(fixture.reports.reports[0].kind, "unhandled");
}

// ---------------------------------------------------------------------------
// Reading — an operator permission, and the grouping that makes the list useful
// ---------------------------------------------------------------------------

TEST(CrashReportServiceTest, APlayerCannotReadCrashReports) {
    CrashFixture fixture;
    ASSERT_TRUE(drogon::sync_wait(fixture.service.submit(crash())).ok());

    const auto groups = drogon::sync_wait(fixture.service.listGroups(anonymousPlayer(), 25, 0));
    const auto reports = drogon::sync_wait(
        fixture.service.list(anonymousPlayer(), launcher::repositories::CrashQuery{}));

    ASSERT_FALSE(groups.ok());
    EXPECT_EQ(groups.error().code, ErrorCode::Forbidden);
    ASSERT_FALSE(reports.ok());
    EXPECT_EQ(reports.error().code, ErrorCode::Forbidden);
}

/// The whole reason groups exist: a thousand reports of one bug is one row an operator can act
/// on, and a thousand rows is not.
TEST(CrashReportServiceTest, ThreeReportsOfOneBugAreOneGroup) {
    CrashFixture fixture;
    for (int index = 0; index < 3; ++index) {
        ASSERT_TRUE(drogon::sync_wait(fixture.service.submit(crash())).ok());
    }
    ASSERT_TRUE(
        drogon::sync_wait(fixture.service.submit(crash("System.InvalidOperationException"))).ok());

    const auto groups =
        drogon::sync_wait(fixture.service.listGroups(operatorWhoReadsCrashes(), 25, 0));

    ASSERT_TRUE(groups.ok()) << groups.error().detail;
    ASSERT_EQ(groups.value().items.size(), 2U);
    EXPECT_EQ(groups.value().total, 2);

    const auto& first = groups.value().items[0];
    EXPECT_EQ(first.occurrences, 3);
    EXPECT_FALSE(first.latestReportId.empty());
}

TEST(CrashReportServiceTest, ReportsCanBeNarrowedToOneBug) {
    CrashFixture fixture;
    ASSERT_TRUE(drogon::sync_wait(fixture.service.submit(crash())).ok());
    const auto other =
        drogon::sync_wait(fixture.service.submit(crash("System.InvalidOperationException")));
    ASSERT_TRUE(other.ok());

    launcher::repositories::CrashQuery query;
    query.fingerprint = other.value().fingerprint;
    const auto page = drogon::sync_wait(fixture.service.list(operatorWhoReadsCrashes(), query));

    ASSERT_TRUE(page.ok()) << page.error().detail;
    ASSERT_EQ(page.value().items.size(), 1U);
    EXPECT_EQ(page.value().items[0].exceptionType, "System.InvalidOperationException");
}

TEST(CrashReportServiceTest, AnUnknownReportIsMissing) {
    CrashFixture fixture;

    const auto found = drogon::sync_wait(
        fixture.service.find(operatorWhoReadsCrashes(), launcher::common::randomUuid()));

    ASSERT_FALSE(found.ok());
    EXPECT_EQ(found.error().code, ErrorCode::NotFound);
}

// Guarded before the value can reach a `$1::uuid` comparison, which PostgreSQL answers with an
// error rather than an empty result.
TEST(CrashReportServiceTest, AnIdentifierThatIsNotOneIsMissingRatherThanAnError) {
    CrashFixture fixture;

    const auto found = drogon::sync_wait(fixture.service.find(operatorWhoReadsCrashes(), "nope"));

    ASSERT_FALSE(found.ok());
    EXPECT_EQ(found.error().code, ErrorCode::NotFound);
}

TEST(CrashReportServiceTest, ThePageSizeIsClampedRatherThanRefused) {
    CrashFixture fixture;
    ASSERT_TRUE(drogon::sync_wait(fixture.service.submit(crash())).ok());

    // A paging control is not worth a failed request; the bound is what stops one call walking
    // the whole table.
    EXPECT_TRUE(
        drogon::sync_wait(fixture.service.listGroups(operatorWhoReadsCrashes(), 100000, -5)).ok());
}

TEST(CrashReportServiceTest, TheSweepRemovesWhatIsPastRetention) {
    CrashFixture fixture;
    const auto stored = drogon::sync_wait(fixture.service.submit(crash()));
    ASSERT_TRUE(stored.ok());
    fixture.reports.expiredIds.push_back(stored.value().id);

    const auto removed = drogon::sync_wait(fixture.service.sweepExpired());

    EXPECT_EQ(removed, 1U);
    EXPECT_TRUE(fixture.reports.reports.empty());
}

} // namespace
