#pragma once

#include <string>
#include <string_view>

namespace launcher::testing {

/// A P-256 key pair the tests own, and a second one that is simply a different key.
///
/// Fixed rather than generated per run so a failure is reproducible, and a *pair* rather than
/// one key because half of what is worth testing here is the refusal: a signature that verifies
/// under some key is not a signature that verifies under *this* key, and only a second key
/// makes that an assertion rather than a claim.
///
/// These are not secrets. They sign nothing outside this test binary, and the real private key
/// deliberately exists nowhere in this repository — see Documentation/hardening-and-deployment.md
/// §6.5.
const char* testReleasePublicKey();
const char* testReleasePrivateKey();
const char* otherReleasePublicKey();
const char* otherReleasePrivateKey();

/// ECDSA-with-SHA-256 over `message`, base64 of the DER signature — exactly what
/// `openssl dgst -sha256 -sign key.pem doc | openssl base64 -A` produces.
///
/// Signing lives in the tests rather than in the server on purpose: nothing in `src/` can
/// produce a signature, which is the property the whole feature rests on. This is the only
/// place in the repository that holds a private key at all.
std::string signWithTestKey(std::string_view message, std::string_view base64PrivateKey);

/// Convenience for the common case: signed with the key the tests configure the server with.
std::string signWithTestKey(std::string_view message);

} // namespace launcher::testing
