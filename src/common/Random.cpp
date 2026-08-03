#include "common/Random.h"

#include <sodium.h>

#include <array>
#include <cstdio>
#include <mutex>
#include <vector>

namespace launcher::common {
namespace {

std::once_flag CRYPTO_INIT_ONCE;
bool CRYPTO_READY = false;

} // namespace

bool initCrypto() {
    std::call_once(CRYPTO_INIT_ONCE, []() {
        // sodium_init returns 1 when another caller already initialised it, which is success.
        CRYPTO_READY = sodium_init() >= 0;
    });
    return CRYPTO_READY;
}

std::string randomUrlSafeToken(std::size_t bytes) {
    if (!initCrypto() || bytes == 0) {
        return {};
    }

    std::vector<unsigned char> buffer(bytes);
    randombytes_buf(buffer.data(), buffer.size());

    const std::size_t encodedSize =
        sodium_base64_encoded_len(buffer.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    std::string encoded(encodedSize, '\0');
    sodium_bin2base64(encoded.data(),
                      encoded.size(),
                      buffer.data(),
                      buffer.size(),
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);

    // sodium_base64_encoded_len counts the trailing NUL; drop it.
    if (!encoded.empty() && encoded.back() == '\0') {
        encoded.pop_back();
    }
    return encoded;
}

std::string randomUuid() {
    if (!initCrypto()) {
        return {};
    }

    std::array<unsigned char, 16> bytes{};
    randombytes_buf(bytes.data(), bytes.size());

    // RFC 4122: version 4, variant 10xx.
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3FU) | 0x80U);

    static constexpr char HEX[] = "0123456789abcdef";
    std::string uuid;
    uuid.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            uuid.push_back('-');
        }
        uuid.push_back(HEX[bytes[i] >> 4U]);
        uuid.push_back(HEX[bytes[i] & 0x0FU]);
    }
    return uuid;
}

} // namespace launcher::common
