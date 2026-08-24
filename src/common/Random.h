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

/// A one-time password for an operator to read out to somebody.
///
/// Unlike every other secret here it has to survive being spoken, typed from a note, or read
/// off a chat message, so it is not `randomUrlSafeToken`: base64url mixes cases and contains
/// characters nobody agrees on the name of. The alphabet is 32 lower-case letters and digits
/// with the four that get confused removed — `l`, `1`, `o` and `0` — and the result is grouped
/// with hyphens. Fifteen symbols over that alphabet is 75 bits, well past guessing, and it is
/// short-lived by construction: the account holding it may do nothing but replace it.
///
/// Drawn with `randombytes_uniform`, which is rejection-sampled, so no symbol is likelier than
/// another — a modulo over 256 would quietly favour the first sixteen.
std::string randomTemporaryPassword();

/// A version 4 UUID, for identifiers the database does not generate.
std::string randomUuid();

} // namespace launcher::common
