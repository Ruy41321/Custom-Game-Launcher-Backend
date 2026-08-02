#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "common/Result.h"

namespace launcher::common {

/// Lowercase hex SHA-256 of an in-memory buffer.
std::string sha256Hex(std::string_view data);

/// Lowercase hex SHA-256 of a file, streamed so that build blobs of arbitrary size can be
/// hashed without being loaded into memory.
Result<std::string> sha256File(const std::filesystem::path& path);

/// Constant-time comparison, for anything that compares a secret or a digest.
bool constantTimeEquals(std::string_view lhs, std::string_view rhs);

} // namespace launcher::common
