#include "support/ReleaseSigning.h"

#include <openssl/evp.h>
#include <openssl/x509.h>
#include <sodium.h>

#include <memory>
#include <vector>

namespace launcher::testing {
namespace {

constexpr const char* TEST_PUBLIC_KEY =
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE+8w7QbVcttzT2zZLHVgU83mnKsL8phBrHgFq3k3q+Qi2/gOhas"
    "O9IUUCnX2BStkHj1v1rx2JTBKNM+XaFgSKDQ==";

constexpr const char* TEST_PRIVATE_KEY =
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgMyYfxBy/Y1cIJ7y63/wjdImyShFyDnIntSEIWJ"
    "l8Z86hRANCAAT7zDtBtVy23NPbNksdWBTzeacqwvymEGseAWreTer5CLb+A6Fqw70hRQKdfYFK2QePW/WvHYlM"
    "Eo0z5doWBIoN";

constexpr const char* OTHER_PUBLIC_KEY =
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEu6UwSKxD+xy1F5JuGC2+YDTPOdzSb1zg76/dv6c3+CDH+7dlRg"
    "+E0y9mjzZV35Y3UTvv8aeumuUgbap/JLD0VQ==";

constexpr const char* OTHER_PRIVATE_KEY =
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgW9yiIA1+HUY44zIo8AwUoSy5Jl7UOlMGkWeAIK"
    "Q14NGhRANCAAS7pTBIrEP7HLUXkm4YLb5gNM853NJvXODvr92/pzf4IMf7t2VGD4TTL2aPNlXfljdRO+/xp66a"
    "5SBtqn8ksPRV";

std::string decode(std::string_view encoded) {
    std::vector<unsigned char> buffer(encoded.size());
    std::size_t length = 0;
    const char* end = nullptr;
    if (sodium_base642bin(buffer.data(),
                          buffer.size(),
                          encoded.data(),
                          encoded.size(),
                          " \r\n\t",
                          &length,
                          &end,
                          sodium_base64_VARIANT_ORIGINAL) != 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(buffer.data()), length);
}

std::string encode(const unsigned char* data, std::size_t length) {
    std::string encoded(sodium_base64_encoded_len(length, sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(encoded.data(), encoded.size(), data, length, sodium_base64_VARIANT_ORIGINAL);
    encoded.pop_back(); // the length includes the trailing NUL
    return encoded;
}

} // namespace

const char* testReleasePublicKey() {
    return TEST_PUBLIC_KEY;
}

const char* testReleasePrivateKey() {
    return TEST_PRIVATE_KEY;
}

const char* otherReleasePublicKey() {
    return OTHER_PUBLIC_KEY;
}

const char* otherReleasePrivateKey() {
    return OTHER_PRIVATE_KEY;
}

std::string signWithTestKey(std::string_view message, std::string_view base64PrivateKey) {
    const auto keyBytes = decode(base64PrivateKey);
    const auto* cursor = reinterpret_cast<const unsigned char*>(keyBytes.data());
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        d2i_PrivateKey(EVP_PKEY_EC, nullptr, &cursor, static_cast<long>(keyBytes.size())),
        &EVP_PKEY_free);
    if (!key) {
        return {};
    }

    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                          &EVP_MD_CTX_free);
    if (!context ||
        EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1) {
        return {};
    }

    std::size_t length = 0;
    if (EVP_DigestSign(context.get(),
                       nullptr,
                       &length,
                       reinterpret_cast<const unsigned char*>(message.data()),
                       message.size()) != 1) {
        return {};
    }

    std::vector<unsigned char> signature(length);
    if (EVP_DigestSign(context.get(),
                       signature.data(),
                       &length,
                       reinterpret_cast<const unsigned char*>(message.data()),
                       message.size()) != 1) {
        return {};
    }

    return encode(signature.data(), length);
}

std::string signWithTestKey(std::string_view message) {
    return signWithTestKey(message, TEST_PRIVATE_KEY);
}

} // namespace launcher::testing
