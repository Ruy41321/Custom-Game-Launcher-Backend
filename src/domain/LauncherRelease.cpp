#include "domain/LauncherRelease.h"

#include <array>
#include <cctype>
#include <tuple>

#include "domain/CanonicalJson.h"
#include "domain/Manifest.h"

namespace launcher::domain {
namespace {

using common::ErrorCode;
using common::VoidResult;

constexpr std::size_t TIMESTAMP_LENGTH = 20; // YYYY-MM-DDTHH:MM:SSZ

bool isDigit(char character) {
    return character >= '0' && character <= '9';
}

/// `YYYY-MM-DDTHH:MM:SSZ` and nothing else — the shape PostgreSQL formats every other timestamp
/// on this API into. Accepting offsets or fractional seconds here would mean the bytes a
/// signature covers could spell one instant several ways, and two documents that differ only in
/// spelling are two rows the unique index cannot see as one.
bool isUtcTimestamp(std::string_view value) {
    if (value.size() != TIMESTAMP_LENGTH) {
        return false;
    }
    static constexpr std::array<std::size_t, 14> DIGITS{
        0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
    for (const auto index : DIGITS) {
        if (!isDigit(value[index])) {
            return false;
        }
    }
    if (value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
        value[16] != ':' || value[19] != 'Z') {
        return false;
    }

    const auto number = [&value](std::size_t offset, std::size_t length) {
        int result = 0;
        for (std::size_t index = offset; index < offset + length; ++index) {
            result = result * 10 + (value[index] - '0');
        }
        return result;
    };

    const int month = number(5, 2);
    const int day = number(8, 2);
    const int hour = number(11, 2);
    const int minute = number(14, 2);
    const int second = number(17, 2);

    // Deliberately not a calendar: 31 February is a date somebody typed wrong and PostgreSQL
    // will refuse it on the way in. What this rules out is the shape being wrong at all.
    return month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour <= 23 && minute <= 59 &&
           second <= 60;
}

} // namespace

std::optional<ReleaseChannel> parseReleaseChannel(std::string_view value) {
    if (value == "stable") {
        return ReleaseChannel::Stable;
    }
    if (value == "beta") {
        return ReleaseChannel::Beta;
    }
    return std::nullopt;
}

std::optional<ReleasePlatform> parseReleasePlatform(std::string_view value) {
    if (value == "windows") {
        return ReleasePlatform::Windows;
    }
    if (value == "linux") {
        return ReleasePlatform::Linux;
    }
    if (value == "macos") {
        return ReleasePlatform::MacOS;
    }
    return std::nullopt;
}

std::optional<ReleaseArch> parseReleaseArch(std::string_view value) {
    if (value == "x64") {
        return ReleaseArch::X64;
    }
    if (value == "arm64") {
        return ReleaseArch::Arm64;
    }
    return std::nullopt;
}

const char* nameFor(ReleaseChannel channel) {
    switch (channel) {
    case ReleaseChannel::Stable:
        return "stable";
    case ReleaseChannel::Beta:
        return "beta";
    }
    return "stable";
}

const char* nameFor(ReleasePlatform platform) {
    switch (platform) {
    case ReleasePlatform::Windows:
        return "windows";
    case ReleasePlatform::Linux:
        return "linux";
    case ReleasePlatform::MacOS:
        return "macos";
    }
    return "windows";
}

const char* nameFor(ReleaseArch arch) {
    switch (arch) {
    case ReleaseArch::X64:
        return "x64";
    case ReleaseArch::Arm64:
        return "arm64";
    }
    return "x64";
}

VoidResult validateReleaseDocument(const ReleaseDocument& document) {
    if (document.schema != RELEASE_DOCUMENT_SCHEMA) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "release document schema must be " +
                                       std::to_string(RELEASE_DOCUMENT_SCHEMA));
    }

    // The full three-component form, always. `0.2` and `0.2.0` are one version written two
    // ways, and the unique index cannot see them as one — so a re-publish under the other
    // spelling would silently become a second row racing to be the newest.
    const auto canonicalVersion = std::to_string(document.version.major) + "." +
                                  std::to_string(document.version.minor) + "." +
                                  std::to_string(document.version.patch);
    if (document.version.text != canonicalVersion) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "version must be written as major.minor.patch, not '" +
                                       document.version.text + "'");
    }

    if (!isSha256Hex(document.artifactSha256)) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "sha256 must be 64 lowercase hexadecimal characters");
    }
    if (document.artifactSize <= 0 || document.artifactSize > MAX_RELEASE_ARTIFACT_BYTES) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "size must be between 1 and " +
                                       std::to_string(MAX_RELEASE_ARTIFACT_BYTES) + " bytes");
    }
    if (!isUtcTimestamp(document.releasedAt)) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "releasedAt must be an ISO 8601 UTC instant, "
                                   "spelled YYYY-MM-DDTHH:MM:SSZ");
    }
    if (document.notes.size() > MAX_LAUNCHER_RELEASE_NOTES_LENGTH) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "notes are longer than " +
                                       std::to_string(MAX_LAUNCHER_RELEASE_NOTES_LENGTH) +
                                       " characters");
    }
    return VoidResult::success();
}

std::string canonicalReleaseDocument(const ReleaseDocument& document) {
    std::string out;
    out.reserve(256 + document.notes.size());

    out.append(R"({"schema":)");
    out.append(std::to_string(document.schema));
    out.append(R"(,"channel":)");
    appendCanonicalJsonString(out, nameFor(document.channel));
    out.append(R"(,"version":)");
    appendCanonicalJsonString(out, document.version.text);
    out.append(R"(,"platform":)");
    appendCanonicalJsonString(out, nameFor(document.platform));
    out.append(R"(,"arch":)");
    appendCanonicalJsonString(out, nameFor(document.arch));
    out.append(R"(,"sha256":)");
    appendCanonicalJsonString(out, document.artifactSha256);
    out.append(R"(,"size":)");
    out.append(std::to_string(document.artifactSize));
    out.append(R"(,"releasedAt":)");
    appendCanonicalJsonString(out, document.releasedAt);
    out.append(R"(,"notes":)");
    appendCanonicalJsonString(out, document.notes);
    out.push_back('}');

    return out;
}

bool isNewerRelease(const Semver& candidate, const Semver& installed) {
    return std::tie(candidate.major, candidate.minor, candidate.patch) >
           std::tie(installed.major, installed.minor, installed.patch);
}

} // namespace launcher::domain
