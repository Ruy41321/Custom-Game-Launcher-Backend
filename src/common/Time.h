#pragma once

#include <cstdint>
#include <string>

namespace launcher::common {

/// ISO 8601 in UTC, `YYYY-MM-DDTHH:MM:SSZ`.
///
/// Timestamps normally reach the API already formatted by PostgreSQL, so this exists for the
/// few the server computes itself — the expiry of a signed URL, which never touches the
/// database. Same shape as the SQL-formatted ones, so a client parses one thing.
std::string formatUnixTimeUtc(int64_t secondsSinceEpoch);

} // namespace launcher::common
