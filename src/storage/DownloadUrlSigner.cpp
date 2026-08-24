#include "storage/DownloadUrlSigner.h"

#include <openssl/evp.h>

#include <array>
#include <memory>
#include <utility>

namespace launcher::storage {
namespace {

constexpr std::size_t MD5_DIGEST_LENGTH = 16;

/// base64url without padding, which is the alphabet nginx's ngx_decode_base64url expects and
/// the one the module's own documentation produces with `tr +/ -_ | tr -d =`.
std::string base64Url(const unsigned char* data, std::size_t length) {
    static constexpr char ALPHABET[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string encoded;
    encoded.reserve((length + 2) / 3 * 4);

    std::size_t index = 0;
    while (index + 2 < length) {
        const uint32_t triple = (static_cast<uint32_t>(data[index]) << 16U) |
                                (static_cast<uint32_t>(data[index + 1]) << 8U) |
                                static_cast<uint32_t>(data[index + 2]);
        encoded.push_back(ALPHABET[(triple >> 18U) & 0x3FU]);
        encoded.push_back(ALPHABET[(triple >> 12U) & 0x3FU]);
        encoded.push_back(ALPHABET[(triple >> 6U) & 0x3FU]);
        encoded.push_back(ALPHABET[triple & 0x3FU]);
        index += 3;
    }

    if (index + 1 == length) {
        const uint32_t triple = static_cast<uint32_t>(data[index]) << 16U;
        encoded.push_back(ALPHABET[(triple >> 18U) & 0x3FU]);
        encoded.push_back(ALPHABET[(triple >> 12U) & 0x3FU]);
    } else if (index + 2 == length) {
        const uint32_t triple = (static_cast<uint32_t>(data[index]) << 16U) |
                                (static_cast<uint32_t>(data[index + 1]) << 8U);
        encoded.push_back(ALPHABET[(triple >> 18U) & 0x3FU]);
        encoded.push_back(ALPHABET[(triple >> 12U) & 0x3FU]);
        encoded.push_back(ALPHABET[(triple >> 6U) & 0x3FU]);
    }

    return encoded;
}

std::string md5Base64Url(std::string_view payload) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    &EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_md5(), nullptr) != 1) {
        return {};
    }

    EVP_DigestUpdate(context.get(), payload.data(), payload.size());

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    EVP_DigestFinal_ex(context.get(), digest.data(), &length);
    if (length != MD5_DIGEST_LENGTH) {
        return {};
    }
    return base64Url(digest.data(), length);
}

std::string withoutTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

/// The path nginx will see. Everything before it — scheme, host, port — is how the client
/// reaches the file server and is deliberately not part of what is signed, so the same
/// deployment can be fronted by a different hostname without invalidating anything.
std::string pathComponentOf(const std::string& url) {
    const auto scheme = url.find("://");
    const auto searchFrom = scheme == std::string::npos ? 0 : scheme + 3;
    const auto path = url.find('/', searchFrom);
    return path == std::string::npos ? std::string{} : withoutTrailingSlash(url.substr(path));
}

} // namespace

DownloadUrlSigner::DownloadUrlSigner(SignedUrlSettings settings)
    : settings_(std::move(settings)),
      basePath_(pathComponentOf(settings_.publicBaseUrl)),
      baseUrl_(withoutTrailingSlash(settings_.publicBaseUrl)) {}

int64_t DownloadUrlSigner::expiryFor(std::chrono::system_clock::time_point now) const {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
    return (seconds + settings_.ttl).count();
}

std::string DownloadUrlSigner::sign(std::string_view storageKey,
                                    int64_t expiresAtUnixSeconds) const {
    const auto expires = std::to_string(expiresAtUnixSeconds);
    const std::string uri = basePath_ + "/" + std::string(storageKey);

    const auto token = md5Base64Url(expires + uri + " " + settings_.secret);
    return baseUrl_ + "/" + std::string(storageKey) + "?token=" + token + "&expires=" + expires;
}

} // namespace launcher::storage
