#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/Result.h"

namespace launcher::domain {

/// One file of a build: where it goes on disk and which blob holds its bytes.
struct ManifestEntry {
    std::string relativePath;
    std::string blobSha256;
    int64_t sizeBytes{0};
    bool isExecutable{false};
};

inline constexpr std::size_t SHA256_HEX_LENGTH = 64;

/// Long enough for any real install tree, short enough that a manifest cannot be used to
/// push a path past the limits of the platforms the client runs on.
inline constexpr std::size_t MAX_RELATIVE_PATH_LENGTH = 1024;

/// A single build cannot be an unbounded number of files: the manifest is parsed into memory
/// and inserted in one statement.
inline constexpr std::size_t MAX_MANIFEST_ENTRIES = 200000;

bool isSha256Hex(std::string_view value);

/// Rejects anything that is not a safe path *inside* the install directory: absolute paths,
/// drive letters, backslashes, `.` and `..` segments, empty segments and control characters.
///
/// The same rules are a CHECK constraint on build_files. Both exist on purpose: this one
/// produces a decent error message, that one guarantees a malicious manifest is unstorable
/// even if this check is ever bypassed.
common::VoidResult validateRelativePath(std::string_view path);

/// Validates a complete file list plus the entrypoint that must appear in it. Duplicate
/// paths are a conflict rather than a last-one-wins merge, because either could be the one
/// the publisher meant.
common::VoidResult validateManifest(const std::vector<ManifestEntry>& entries,
                                    std::string_view entrypointRelativePath);

/// Byte-exact serialisation of a manifest.
///
/// `builds.manifest_sha256` is the SHA-256 of exactly these bytes, and the manifest endpoint
/// serves exactly these bytes, so a client verifies what it downloaded by hashing the
/// response — it never has to reproduce a canonical form of its own. Entries are sorted by
/// path, keys are emitted in a fixed order and there is no insignificant whitespace.
///
/// The build id is deliberately absent: two builds with identical content then carry
/// identical manifest hashes, which is what makes the hash usable as a content identity.
std::string canonicalManifestDocument(std::vector<ManifestEntry> entries,
                                      std::string_view entrypointRelativePath,
                                      std::string_view defaultLaunchArgs);

} // namespace launcher::domain
