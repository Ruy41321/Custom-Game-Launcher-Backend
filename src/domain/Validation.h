#pragma once

#include <string>
#include <string_view>

#include "common/Result.h"

namespace launcher::domain {

/// Minimum password length. Length is the only composition rule enforced: mandatory
/// symbol/digit classes measurably push users towards predictable substitutions without
/// buying entropy, so this follows the NIST 800-63B guidance of a long minimum instead.
inline constexpr std::size_t MIN_PASSWORD_LENGTH = 12;

/// Upper bound so a multi-megabyte "password" cannot be turned into an Argon2id
/// denial-of-service.
inline constexpr std::size_t MAX_PASSWORD_LENGTH = 256;

inline constexpr std::size_t MAX_EMAIL_LENGTH = 254; // RFC 5321 path limit

inline constexpr std::size_t MIN_DISPLAY_NAME_LENGTH = 2;
inline constexpr std::size_t MAX_DISPLAY_NAME_LENGTH = 64;

/// Trims surrounding whitespace and lowercases. Emails are stored in a citext column, so
/// this is about consistent display and comparison, not correctness of the unique index.
std::string normalizeEmail(std::string_view email);

std::string trim(std::string_view text);

common::VoidResult validateEmail(std::string_view email);

common::VoidResult validatePassword(std::string_view password);

common::VoidResult validateDisplayName(std::string_view displayName);

/// True for the canonical 8-4-4-4-12 hexadecimal form.
///
/// Callers check this before a value reaches a `$n::uuid` comparison: PostgreSQL raises on a
/// malformed uuid literal, which would turn a mistyped identifier into a 500 instead of the
/// 404 it actually is.
bool isUuid(std::string_view value);

} // namespace launcher::domain
