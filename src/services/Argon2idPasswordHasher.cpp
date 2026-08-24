#include <sodium.h>

#include <vector>

#include "common/Random.h"
#include "services/PasswordHasher.h"

namespace launcher::services {
namespace {

using common::ErrorCode;
using common::Result;

constexpr const char* DUMMY_PASSWORD = "timing-equalisation-placeholder";

} // namespace

Argon2idPasswordHasher::Argon2idPasswordHasher(PasswordHashingSettings settings)
    : operationsLimit_(settings.operationsLimit != 0 ? settings.operationsLimit
                                                     : crypto_pwhash_OPSLIMIT_INTERACTIVE),
      memoryLimitBytes_(settings.memoryLimitBytes != 0 ? settings.memoryLimitBytes
                                                       : crypto_pwhash_MEMLIMIT_INTERACTIVE) {
    common::initCrypto();
}

Result<std::string> Argon2idPasswordHasher::hash(std::string_view password) const {
    if (!common::initCrypto()) {
        return Result<std::string>::failure(ErrorCode::Internal, "libsodium is not available");
    }

    std::vector<char> encoded(crypto_pwhash_STRBYTES, '\0');

    // ALG_ARGON2ID13 is pinned rather than left to ALG_DEFAULT: the default could change
    // between libsodium versions, and stored hashes must stay verifiable.
    const int status = crypto_pwhash_str_alg(encoded.data(),
                                             password.data(),
                                             password.size(),
                                             operationsLimit_,
                                             memoryLimitBytes_,
                                             crypto_pwhash_ALG_ARGON2ID13);
    if (status != 0) {
        // The documented failure is running out of memory for the requested memlimit.
        return Result<std::string>::failure(ErrorCode::Internal,
                                            "password hashing failed (out of memory?)");
    }

    return Result<std::string>::success(std::string(encoded.data()));
}

bool Argon2idPasswordHasher::verify(std::string_view password,
                                    const std::string& encodedHash) const {
    if (!common::initCrypto() || encodedHash.empty() ||
        encodedHash.size() >= crypto_pwhash_STRBYTES) {
        return false;
    }

    // crypto_pwhash_str_verify requires a NUL-terminated hash, which std::string guarantees.
    return crypto_pwhash_str_verify(encodedHash.c_str(), password.data(), password.size()) == 0;
}

bool Argon2idPasswordHasher::needsRehash(const std::string& encodedHash) const {
    if (!common::initCrypto() || encodedHash.empty() ||
        encodedHash.size() >= crypto_pwhash_STRBYTES) {
        return true;
    }

    // Returns 0 when the parameters already match, 1 when they are weaker, -1 when the
    // string is not a recognisable hash. Both non-zero cases mean "replace it".
    return crypto_pwhash_str_needs_rehash(
               encodedHash.c_str(), operationsLimit_, memoryLimitBytes_) != 0;
}

void Argon2idPasswordHasher::performDummyHash() const {
    (void)hash(DUMMY_PASSWORD);
}

} // namespace launcher::services
