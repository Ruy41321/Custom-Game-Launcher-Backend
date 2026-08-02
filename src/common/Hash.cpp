#include "common/Hash.h"

#include <openssl/evp.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>

namespace launcher::common {
namespace {

constexpr std::size_t FILE_CHUNK_BYTES = 1U << 20U;

using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

DigestContext makeDigestContext() {
    DigestContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (context && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        context.reset();
    }
    return context;
}

std::string toHex(const unsigned char* digest, unsigned int length) {
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(static_cast<std::size_t>(length) * 2);
    for (unsigned int i = 0; i < length; ++i) {
        hex.push_back(HEX_DIGITS[digest[i] >> 4U]);
        hex.push_back(HEX_DIGITS[digest[i] & 0x0FU]);
    }
    return hex;
}

} // namespace

std::string sha256Hex(std::string_view data) {
    auto context = makeDigestContext();
    if (!context) {
        return {};
    }

    EVP_DigestUpdate(context.get(), data.data(), data.size());

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    EVP_DigestFinal_ex(context.get(), digest.data(), &length);
    return toHex(digest.data(), length);
}

Result<std::string> sha256File(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return Result<std::string>::failure(ErrorCode::NotFound,
                                            "cannot open file for hashing: " + path.string());
    }

    auto context = makeDigestContext();
    if (!context) {
        return Result<std::string>::failure(ErrorCode::Internal,
                                            "failed to initialise SHA-256 context");
    }

    std::vector<char> buffer(FILE_CHUNK_BYTES);
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = static_cast<std::size_t>(stream.gcount());
        if (read > 0) {
            EVP_DigestUpdate(context.get(), buffer.data(), read);
        }
    }

    if (stream.bad()) {
        return Result<std::string>::failure(ErrorCode::Internal,
                                            "read error while hashing " + path.string());
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    EVP_DigestFinal_ex(context.get(), digest.data(), &length);
    return Result<std::string>::success(toHex(digest.data(), length));
}

bool constantTimeEquals(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        difference |= static_cast<unsigned char>(lhs[i]) ^ static_cast<unsigned char>(rhs[i]);
    }
    return difference == 0;
}

} // namespace launcher::common
