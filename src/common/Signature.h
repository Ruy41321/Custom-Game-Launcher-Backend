#pragma once

#include <string>
#include <string_view>

#include "common/Result.h"

namespace launcher::common {

/// Detached signature verification, for the one document on this server that is signed
/// somewhere else entirely.
///
/// **ECDSA over NIST P-256 with SHA-256, and the algorithm is pinned rather than read out of
/// the key.** Ed25519 is the better modern choice and was rejected for a reason that lives
/// entirely on the other side of the wire: .NET 9 has no Ed25519 in its base class library, so
/// the launcher would have to carry either a native libsodium binding across four self-contained
/// runtime identifiers or a managed crypto library — in a client whose maintainers have refused
/// a dependency over thirty lines of test code. P-256 costs nothing on either side: OpenSSL is
/// already linked here for the download-URL signatures, and `System.Security.Cryptography.ECDsa`
/// is in the client's runtime. The two known weaknesses of ECDSA do not reach this use: nonce
/// quality is a property of *signing*, which happens on somebody's own machine a few times a
/// year, and malleability matters when a signature is an identifier, which this one never is.
///
/// Pinning matters as much as the choice. If the algorithm were taken from whatever the
/// configured key happens to be, a deployment could be given an RSA key that this server would
/// verify happily and the client would not understand at all — a launcher that stops updating
/// for a reason nothing reports. Anything that is not a P-256 key is refused when the
/// configuration is read.
///
/// Neither function throws, and neither reports *why* a signature failed beyond "it did not
/// verify": there is nothing useful to distinguish, and the interesting failures — a key that
/// is not a key, a signature that is not base64 — are configuration and publishing mistakes
/// that are named exactly because they happen before anything is served.

/// Rejects anything that is not a base64-encoded DER SubjectPublicKeyInfo holding a P-256
/// public key. Called when the configuration is validated, so a mistyped key is a start-up
/// failure naming the variable rather than a feature that silently serves nothing.
///
/// The value is the body of a `-----BEGIN PUBLIC KEY-----` block. Whitespace and line breaks
/// are ignored, so the PEM body pastes in either wrapped or on one line.
VoidResult validateP256PublicKey(std::string_view base64PublicKey);

/// True when `base64Signature` is a valid DER ECDSA-with-SHA-256 signature over `message`
/// under `base64PublicKey`.
VoidResult verifyP256Signature(std::string_view base64PublicKey,
                               std::string_view base64Signature,
                               std::string_view message);

/// Standard-alphabet base64 decoding, tolerating whitespace and rejecting trailing rubbish.
Result<std::string> decodeBase64(std::string_view encoded);

} // namespace launcher::common
