#pragma once

#include <cstddef>
#include <string>

namespace launcher::common {

/// Initialises libsodium. Idempotent and thread-safe; every entry point that uses libsodium
/// calls it, so there is no ordering requirement between subsystems.
bool initCrypto();

/// Cryptographically secure random bytes, base64url encoded without padding.
///
/// Used for refresh tokens and for email verification / password reset links, so the
/// generator must be the CSPRNG — never `rand()` or a `mt19937` seeded from the clock.
/// 32 bytes gives 256 bits of entropy, which is far beyond guessable.
std::string randomUrlSafeToken(std::size_t bytes = 32);

/// A version 4 UUID, for identifiers the database does not generate.
std::string randomUuid();

} // namespace launcher::common
