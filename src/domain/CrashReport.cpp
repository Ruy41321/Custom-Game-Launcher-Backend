#include "domain/CrashReport.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

#include "common/Hash.h"
#include "domain/Catalog.h"
#include "domain/Validation.h"

namespace launcher::domain {
namespace {

using common::ErrorCode;
using common::VoidResult;

VoidResult checkLength(std::string_view value, std::size_t limit, const char* field) {
    if (value.size() > limit) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   std::string(field) + " must be at most " +
                                       std::to_string(limit) + " characters");
    }
    return VoidResult::success();
}

/// Whether a character is part of a name rather than of a number or an address. Everything
/// else is what moves between builds, and dropping it is what makes a fingerprint survive a
/// recompilation.
bool isNameCharacter(unsigned char character) {
    return std::isalpha(character) != 0 || character == '.' || character == '_' ||
           character == ':' || character == '<' || character == '>' || character == '+';
}

bool allDigits(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char digit) {
        return std::isdigit(digit) != 0;
    });
}

/// An ISO 8601 instant, strictly enough that nothing malformed reaches a `::timestamptz` cast.
/// Checked here rather than left to PostgreSQL, which answers a bad literal with an error that
/// surfaces as a 500 for what is plainly the caller's mistake.
VoidResult validateOccurredAt(std::string_view value) {
    constexpr std::size_t DATE_LENGTH = 10;
    constexpr std::size_t SECONDS_END = 19; // YYYY-MM-DDTHH:MM:SS
    const auto malformed =
        VoidResult::failure(ErrorCode::InvalidInput,
                            "occurredAt must be an ISO 8601 instant, e.g. 2026-08-05T12:00:00Z");

    if (value.size() < SECONDS_END + 1 || value[DATE_LENGTH] != 'T' || value[13] != ':' ||
        value[16] != ':') {
        return malformed;
    }
    if (auto date = validateReleaseDate(value.substr(0, DATE_LENGTH)); !date.ok()) {
        return malformed;
    }
    if (!allDigits(value.substr(11, 2)) || !allDigits(value.substr(14, 2)) ||
        !allDigits(value.substr(17, 2))) {
        return malformed;
    }

    auto zone = value.substr(SECONDS_END);
    if (!zone.empty() && zone.front() == '.') {
        const auto digits = zone.substr(1);
        const auto end = digits.find_first_not_of("0123456789");
        if (end == 0) {
            return malformed;
        }
        zone = end == std::string_view::npos ? std::string_view{} : digits.substr(end);
    }

    // A zone is required: an instant with none is a local time, and storing one as if it were
    // UTC silently moves a crash by however many hours the reporter happens to live from it.
    if (zone == "Z") {
        return VoidResult::success();
    }
    if (zone.size() == 6 && (zone[0] == '+' || zone[0] == '-') && zone[3] == ':' &&
        allDigits(zone.substr(1, 2)) && allDigits(zone.substr(4, 2))) {
        return VoidResult::success();
    }
    return malformed;
}

} // namespace

VoidResult validateCrashReport(const NewCrashReport& report) {
    if (trim(report.kind).empty()) {
        return VoidResult::failure(ErrorCode::InvalidInput, "kind must not be empty");
    }
    if (auto check = checkLength(report.kind, MAX_CRASH_KIND_LENGTH, "kind"); !check.ok()) {
        return check;
    }
    if (auto check = checkLength(report.launcherVersion, MAX_CRASH_VERSION_LENGTH, "version");
        !check.ok()) {
        return check;
    }
    if (auto check = checkLength(report.platform, MAX_CRASH_PLATFORM_LENGTH, "platform");
        !check.ok()) {
        return check;
    }
    if (auto check =
            checkLength(report.exceptionType, MAX_CRASH_EXCEPTION_TYPE_LENGTH, "exceptionType");
        !check.ok()) {
        return check;
    }
    if (auto check = checkLength(report.message, MAX_CRASH_MESSAGE_LENGTH, "message");
        !check.ok()) {
        return check;
    }
    if (auto check = checkLength(report.stackTrace, MAX_CRASH_STACK_LENGTH, "stackTrace");
        !check.ok()) {
        return check;
    }

    if (auto check = validateOccurredAt(report.occurredAt); !check.ok()) {
        return check;
    }

    return VoidResult::success();
}

std::string normalizeStackForFingerprint(std::string_view stackTrace) {
    // Keeps the names of methods, types and namespaces and drops everything else: digits,
    // offsets, addresses and the punctuation around them. Two builds of the same code produce
    // the same frames with different numbers, and a fingerprint that noticed the numbers would
    // report every rebuild as a brand new bug.
    std::string normalized;
    normalized.reserve(stackTrace.size());

    bool lastWasSeparator = true;
    for (const char raw : stackTrace) {
        const auto character = static_cast<unsigned char>(raw);
        if (isNameCharacter(character)) {
            normalized.push_back(static_cast<char>(std::tolower(character)));
            lastWasSeparator = false;
        } else if (!lastWasSeparator) {
            normalized.push_back(' ');
            lastWasSeparator = true;
        }
    }

    while (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }
    return normalized;
}

std::string crashFingerprint(const NewCrashReport& report) {
    // The message is deliberately *not* part of it: "could not open D:\\Games\\a.pak" and
    // "could not open C:\\Games\\b.pak" are one bug, and folding the message in would split
    // every path-carrying failure into as many bugs as there are machines.
    return common::sha256Hex(trim(report.exceptionType) + "\n" +
                             normalizeStackForFingerprint(report.stackTrace));
}

} // namespace launcher::domain
