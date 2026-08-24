#include <gtest/gtest.h>

#include <string>

#include "domain/CrashReport.h"

namespace {

using launcher::common::ErrorCode;
using launcher::domain::CrashReport;
using launcher::domain::NewCrashReport;

NewCrashReport valid() {
    NewCrashReport report;
    report.kind = "unhandled";
    report.occurredAt = "2026-08-05T12:00:00Z";
    report.launcherVersion = "0.1.0";
    report.platform = "Microsoft Windows NT 10.0.26200.0";
    report.exceptionType = "System.IO.IOException";
    report.message = "the file is in use";
    report.stackTrace =
        "   at GameLauncher.Downloads.BlobFetcher.FetchAsync() in Fetcher.cs:line 42";
    return report;
}

// ---------------------------------------------------------------------------
// Validation — every field arrives from an unauthenticated caller
// ---------------------------------------------------------------------------

TEST(CrashReportDomainTest, AcceptsAWellFormedReport) {
    EXPECT_TRUE(launcher::domain::validateCrashReport(valid()).ok());
}

TEST(CrashReportDomainTest, RefusesAReportWithNoKind) {
    auto report = valid();
    report.kind = "   ";

    const auto checked = launcher::domain::validateCrashReport(report);

    ASSERT_FALSE(checked.ok());
    EXPECT_EQ(checked.error().code, ErrorCode::InvalidInput);
}

TEST(CrashReportDomainTest, RefusesAStackTraceLongerThanTheServerWillStore) {
    auto report = valid();
    report.stackTrace = std::string(launcher::domain::MAX_CRASH_STACK_LENGTH + 1, 'x');

    const auto checked = launcher::domain::validateCrashReport(report);

    ASSERT_FALSE(checked.ok());
    EXPECT_EQ(checked.error().code, ErrorCode::InvalidInput);
}

TEST(CrashReportDomainTest, RefusesAMessageLongerThanTheLimit) {
    auto report = valid();
    report.message = std::string(launcher::domain::MAX_CRASH_MESSAGE_LENGTH + 1, 'x');

    EXPECT_FALSE(launcher::domain::validateCrashReport(report).ok());
}

/// A malformed timestamp reaching a `::timestamptz` cast is an error the driver raises, which
/// surfaces as a 500 for what is plainly the caller's mistake.
TEST(CrashReportDomainTest, RefusesATimestampThatIsNotOne) {
    for (const char* value : {"",
                              "yesterday",
                              "2026-08-05",
                              "2026-13-05T12:00:00Z",
                              "2026-08-05 12:00:00Z",
                              "2026-08-05T12:00:00"}) {
        auto report = valid();
        report.occurredAt = value;
        EXPECT_FALSE(launcher::domain::validateCrashReport(report).ok()) << "accepted: " << value;
    }
}

// A local time stored as if it were UTC moves a crash by however many hours the reporter
// happens to live from it, which is why a zone is required rather than assumed.
TEST(CrashReportDomainTest, AcceptsBothZuluAndAnOffset) {
    for (const char* value :
         {"2026-08-05T12:00:00Z", "2026-08-05T12:00:00.123Z", "2026-08-05T12:00:00+02:00"}) {
        auto report = valid();
        report.occurredAt = value;
        EXPECT_TRUE(launcher::domain::validateCrashReport(report).ok()) << "refused: " << value;
    }
}

// ---------------------------------------------------------------------------
// The fingerprint — what makes a thousand reports into nine bugs
// ---------------------------------------------------------------------------

TEST(CrashReportDomainTest, TheSameCrashFingerprintsTheSameWay) {
    EXPECT_EQ(launcher::domain::crashFingerprint(valid()),
              launcher::domain::crashFingerprint(valid()));
}

/// The whole reason the stack is normalised: line numbers move with every build, and a
/// fingerprint that noticed them would report every recompilation as a brand new bug.
TEST(CrashReportDomainTest, ARebuildThatMovedTheLineNumbersIsTheSameBug) {
    auto before = valid();
    auto after = valid();
    after.stackTrace =
        "   at GameLauncher.Downloads.BlobFetcher.FetchAsync() in Fetcher.cs:line 117";

    EXPECT_EQ(launcher::domain::crashFingerprint(before),
              launcher::domain::crashFingerprint(after));
}

/// And the reason the message is left out: two machines failing on two paths are one bug, and
/// folding the message in would split it into as many bugs as there are machines.
TEST(CrashReportDomainTest, TwoMachinesFailingOnDifferentPathsAreOneBug) {
    auto first = valid();
    first.message = R"(could not open D:\Games\Orbital\a.pak)";
    auto second = valid();
    second.message = R"(could not open C:\Users\someone\Games\b.pak)";

    EXPECT_EQ(launcher::domain::crashFingerprint(first),
              launcher::domain::crashFingerprint(second));
}

TEST(CrashReportDomainTest, ADifferentExceptionIsADifferentBug) {
    auto other = valid();
    other.exceptionType = "System.InvalidOperationException";

    EXPECT_NE(launcher::domain::crashFingerprint(valid()),
              launcher::domain::crashFingerprint(other));
}

TEST(CrashReportDomainTest, ADifferentStackIsADifferentBug) {
    auto other = valid();
    other.stackTrace = "   at GameLauncher.Installs.SqliteInstallStore.SaveAsync()";

    EXPECT_NE(launcher::domain::crashFingerprint(valid()),
              launcher::domain::crashFingerprint(other));
}

TEST(CrashReportDomainTest, TheFingerprintIsAHexDigest) {
    const auto fingerprint = launcher::domain::crashFingerprint(valid());

    EXPECT_EQ(fingerprint.size(), 64U);
    EXPECT_EQ(fingerprint.find_first_not_of("0123456789abcdef"), std::string::npos);
}

TEST(CrashReportDomainTest, NormalisingDropsTheNumbersAndKeepsTheNames) {
    const auto normalized = launcher::domain::normalizeStackForFingerprint(
        "   at Namespace.Type.Method() in C:\\src\\File.cs:line 42");

    EXPECT_NE(normalized.find("namespace.type.method"), std::string::npos);
    EXPECT_EQ(normalized.find("42"), std::string::npos);
}

} // namespace
