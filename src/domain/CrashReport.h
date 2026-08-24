#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/Result.h"

namespace launcher::domain {

/// Field limits for an incoming crash report.
///
/// A stack trace is the only field that is legitimately long, and every one of these is a cap
/// on what an unauthenticated caller can make the server store. They are generous enough that
/// a real crash arrives whole and small enough that a thousand of them are still a few
/// megabytes.
inline constexpr std::size_t MAX_CRASH_KIND_LENGTH = 40;
inline constexpr std::size_t MAX_CRASH_VERSION_LENGTH = 40;
inline constexpr std::size_t MAX_CRASH_PLATFORM_LENGTH = 120;
inline constexpr std::size_t MAX_CRASH_EXCEPTION_TYPE_LENGTH = 200;
inline constexpr std::size_t MAX_CRASH_MESSAGE_LENGTH = 2000;
inline constexpr std::size_t MAX_CRASH_STACK_LENGTH = 16000;

/// What a launcher sends. Nothing identifies the account or the machine, by design: see the
/// comment at the head of migration 0004.
struct NewCrashReport {
    std::string kind;
    /// ISO 8601, as the client observed it. Kept apart from `receivedAt` because a report is
    /// written to disk and sent on the *next* start, so the two can be days apart.
    std::string occurredAt;
    std::string launcherVersion;
    std::string platform;
    std::string exceptionType;
    std::string message;
    std::string stackTrace;
};

struct CrashReport {
    std::string id;
    std::string kind;
    std::string occurredAt;
    std::string launcherVersion;
    std::string platform;
    std::string exceptionType;
    std::string message;
    std::string stackTrace;
    std::string fingerprint;
    std::string receivedAt;
};

/// One distinct crash, and how much of it has arrived. What turns a thousand reports into the
/// nine bugs they actually are.
struct CrashGroup {
    std::string fingerprint;
    std::string exceptionType;
    std::string message;
    int64_t occurrences{0};
    std::string firstSeenAt;
    std::string lastSeenAt;
    /// The most recent report carrying this fingerprint, so the console can open one.
    std::string latestReportId;
};

/// Trims, caps and rejects what cannot be stored. A crash report arrives from an
/// unauthenticated client, so every field is treated as hostile until this has run.
common::VoidResult validateCrashReport(const NewCrashReport& report);

/// The identity of a *bug*, as opposed to a report of one.
///
/// Computed server-side from the exception type and the shape of the stack, so that two client
/// versions cannot disagree about what "the same crash" means and no caller can choose its own
/// grouping. The stack is normalised first: line numbers, addresses and offsets move with every
/// build, and a fingerprint that changed on recompilation would group nothing.
std::string crashFingerprint(const NewCrashReport& report);

/// Strips what varies between builds and machines out of a stack trace, leaving the frames.
/// Exposed for its tests; `crashFingerprint` is the only production caller.
std::string normalizeStackForFingerprint(std::string_view stackTrace);

} // namespace launcher::domain
