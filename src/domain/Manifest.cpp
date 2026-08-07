#include "domain/Manifest.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include "domain/CanonicalJson.h"

namespace launcher::domain {
namespace {

using common::ErrorCode;
using common::VoidResult;

bool isLowercaseHexDigit(char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
}

/// The escaper moved to domain/CanonicalJson.h once a second canonical document — a launcher
/// release — came to depend on producing byte-identical output. Both hashes cover exactly what
/// it emits, so the two must not be able to drift apart.
void appendJsonString(std::string& out, std::string_view value) {
    appendCanonicalJsonString(out, value);
}

} // namespace

bool isSha256Hex(std::string_view value) {
    if (value.size() != SHA256_HEX_LENGTH) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), isLowercaseHexDigit);
}

VoidResult validateRelativePath(std::string_view path) {
    if (path.empty()) {
        return VoidResult::failure(ErrorCode::InvalidInput, "a file path must not be empty");
    }
    if (path.size() > MAX_RELATIVE_PATH_LENGTH) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "file path is longer than " +
                                       std::to_string(MAX_RELATIVE_PATH_LENGTH) + " characters");
    }
    if (path.front() == '/') {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "file paths must be relative to the install directory: " +
                                       std::string(path));
    }
    if (path.find('\\') != std::string_view::npos) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "file paths must use '/' as the separator: " +
                                       std::string(path));
    }
    // A Windows drive prefix such as "C:relative" is not caught by the leading-slash rule.
    if (path.size() >= 2 && path[1] == ':' &&
        std::isalpha(static_cast<unsigned char>(path[0])) != 0) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "file paths must not be absolute: " + std::string(path));
    }

    std::size_t start = 0;
    while (start <= path.size()) {
        const auto separator = path.find('/', start);
        const auto segment = path.substr(
            start,
            separator == std::string_view::npos ? std::string_view::npos : separator - start);

        if (segment.empty()) {
            return VoidResult::failure(ErrorCode::InvalidInput,
                                       "file paths must not contain empty segments: " +
                                           std::string(path));
        }
        if (segment == "." || segment == "..") {
            return VoidResult::failure(ErrorCode::InvalidInput,
                                       "file paths must not contain '.' or '..' segments: " +
                                           std::string(path));
        }
        for (const char character : segment) {
            if (static_cast<unsigned char>(character) < 0x20) {
                return VoidResult::failure(ErrorCode::InvalidInput,
                                           "file paths must not contain control characters");
            }
        }

        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }

    return VoidResult::success();
}

VoidResult validateManifest(const std::vector<ManifestEntry>& entries,
                            std::string_view entrypointRelativePath) {
    if (entries.empty()) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "a build must contain at least one file");
    }
    if (entries.size() > MAX_MANIFEST_ENTRIES) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "a build may contain at most " +
                                       std::to_string(MAX_MANIFEST_ENTRIES) + " files");
    }

    if (auto check = validateRelativePath(entrypointRelativePath); !check.ok()) {
        return check;
    }

    std::unordered_set<std::string> seen;
    seen.reserve(entries.size());

    for (const auto& entry : entries) {
        if (auto check = validateRelativePath(entry.relativePath); !check.ok()) {
            return check;
        }
        if (!isSha256Hex(entry.blobSha256)) {
            return VoidResult::failure(ErrorCode::InvalidInput,
                                       "sha256 must be 64 lowercase hex characters, for " +
                                           entry.relativePath);
        }
        if (entry.sizeBytes < 0) {
            return VoidResult::failure(ErrorCode::InvalidInput,
                                       "size must not be negative, for " + entry.relativePath);
        }
        if (!seen.insert(entry.relativePath).second) {
            return VoidResult::failure(ErrorCode::Conflict,
                                       "the manifest lists " + entry.relativePath + " twice");
        }
    }

    if (seen.find(std::string(entrypointRelativePath)) == seen.end()) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "the entrypoint " + std::string(entrypointRelativePath) +
                                       " is not one of the files in the manifest");
    }

    return VoidResult::success();
}

std::string canonicalManifestDocument(std::vector<ManifestEntry> entries,
                                      std::string_view entrypointRelativePath,
                                      std::string_view defaultLaunchArgs) {
    std::sort(
        entries.begin(), entries.end(), [](const ManifestEntry& lhs, const ManifestEntry& rhs) {
            return lhs.relativePath < rhs.relativePath;
        });

    std::string document;
    document.reserve(entries.size() * 128);

    document.append(R"({"schema":1,"entrypoint":)");
    appendJsonString(document, entrypointRelativePath);
    document.append(R"(,"launchArgs":)");
    appendJsonString(document, defaultLaunchArgs);
    document.append(R"(,"files":[)");

    bool first = true;
    for (const auto& entry : entries) {
        if (!first) {
            document.push_back(',');
        }
        first = false;

        document.append(R"({"path":)");
        appendJsonString(document, entry.relativePath);
        document.append(R"(,"sha256":)");
        appendJsonString(document, entry.blobSha256);
        document.append(R"(,"size":)");
        document.append(std::to_string(entry.sizeBytes));
        document.append(R"(,"executable":)");
        document.append(entry.isExecutable ? "true" : "false");
        document.push_back('}');
    }

    document.append("]}");
    return document;
}

} // namespace launcher::domain
