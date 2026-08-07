#include "common/Signature.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <sodium.h>

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace launcher::common {
namespace {

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

/// A SubjectPublicKeyInfo for P-256 is 91 bytes and a DER ECDSA signature is at most 72. The
/// bounds exist so a configuration value cannot size an allocation.
constexpr std::size_t MAX_ENCODED_LENGTH = 4096;

constexpr int P256_KEY_BITS = 256;

/// OpenSSL 3 answers with the group's short name, and which spelling it uses has changed
/// between minor releases. Both are the same curve.
bool isP256GroupName(std::string_view name) {
    return name == "prime256v1" || name == "P-256" || name == "secp256r1";
}

Result<PkeyPtr> loadP256PublicKey(std::string_view base64PublicKey) {
    if (base64PublicKey.empty()) {
        return Result<PkeyPtr>::failure(ErrorCode::InvalidInput, "the public key is empty");
    }

    auto decoded = decodeBase64(base64PublicKey);
    if (!decoded.ok()) {
        return Result<PkeyPtr>::failure(
            ErrorCode::InvalidInput,
            "the public key is not base64: " + decoded.error().detail +
                " (expected the body of a PEM 'BEGIN PUBLIC KEY' block, without its two "
                "delimiter lines)");
    }

    const auto keyBytes = std::move(decoded).value();
    const auto* cursor = reinterpret_cast<const unsigned char*>(keyBytes.data());
    PkeyPtr key(d2i_PUBKEY(nullptr, &cursor, static_cast<long>(keyBytes.size())), &EVP_PKEY_free);
    if (!key) {
        return Result<PkeyPtr>::failure(
            ErrorCode::InvalidInput, "the public key is not a DER SubjectPublicKeyInfo document");
    }

    if (EVP_PKEY_is_a(key.get(), "EC") != 1 || EVP_PKEY_get_bits(key.get()) != P256_KEY_BITS) {
        return Result<PkeyPtr>::failure(
            ErrorCode::InvalidInput,
            "the public key is not an elliptic-curve P-256 key; generate one with "
            "'openssl ecparam -name prime256v1 -genkey -noout -out release-signing.key'");
    }

    std::array<char, 64> groupName{};
    std::size_t groupNameLength = 0;
    if (EVP_PKEY_get_utf8_string_param(key.get(),
                                       OSSL_PKEY_PARAM_GROUP_NAME,
                                       groupName.data(),
                                       groupName.size(),
                                       &groupNameLength) != 1 ||
        !isP256GroupName(std::string_view(groupName.data(), groupNameLength))) {
        return Result<PkeyPtr>::failure(ErrorCode::InvalidInput,
                                        "the public key is on a curve other than P-256");
    }

    return Result<PkeyPtr>::success(std::move(key));
}

} // namespace

Result<std::string> decodeBase64(std::string_view encoded) {
    if (encoded.size() > MAX_ENCODED_LENGTH) {
        return Result<std::string>::failure(ErrorCode::InvalidInput, "the value is too long");
    }

    // Line breaks and spaces are ignored, so a PEM body pastes in wrapped at 64 columns or
    // joined onto one line — both are what somebody's clipboard actually holds.
    std::vector<unsigned char> buffer(encoded.size());
    std::size_t decodedLength = 0;
    const char* end = nullptr;
    if (sodium_base642bin(buffer.data(),
                          buffer.size(),
                          encoded.data(),
                          encoded.size(),
                          " \r\n\t",
                          &decodedLength,
                          &end,
                          sodium_base64_VARIANT_ORIGINAL) != 0) {
        return Result<std::string>::failure(ErrorCode::InvalidInput,
                                            "the value is not valid base64");
    }

    // sodium stops at the first character it can neither decode nor ignore and reports where.
    // Without this a signature with a stray character silently decodes to a shorter one, which
    // then fails verification for a reason nothing names.
    if (end != encoded.data() + encoded.size()) {
        return Result<std::string>::failure(
            ErrorCode::InvalidInput, "the value has trailing characters that are not base64");
    }

    return Result<std::string>::success(
        std::string(reinterpret_cast<const char*>(buffer.data()), decodedLength));
}

VoidResult validateP256PublicKey(std::string_view base64PublicKey) {
    auto key = loadP256PublicKey(base64PublicKey);
    if (!key.ok()) {
        return VoidResult::failure(key.error());
    }
    return VoidResult::success();
}

VoidResult verifyP256Signature(std::string_view base64PublicKey,
                               std::string_view base64Signature,
                               std::string_view message) {
    auto key = loadP256PublicKey(base64PublicKey);
    if (!key.ok()) {
        return VoidResult::failure(key.error());
    }

    auto signature = decodeBase64(base64Signature);
    if (!signature.ok()) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "the signature is not base64: " + signature.error().detail);
    }
    const auto signatureBytes = std::move(signature).value();
    if (signatureBytes.empty()) {
        return VoidResult::failure(ErrorCode::InvalidInput, "the signature is empty");
    }

    MdCtxPtr context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context || EVP_DigestVerifyInit(
                        context.get(), nullptr, EVP_sha256(), nullptr, key.value().get()) != 1) {
        return VoidResult::failure(ErrorCode::Internal,
                                   "the signature could not be checked: OpenSSL refused to "
                                   "start a verification");
    }

    const auto verified =
        EVP_DigestVerify(context.get(),
                         reinterpret_cast<const unsigned char*>(signatureBytes.data()),
                         signatureBytes.size(),
                         reinterpret_cast<const unsigned char*>(message.data()),
                         message.size());
    if (verified != 1) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "the signature does not match this document and key");
    }
    return VoidResult::success();
}

} // namespace launcher::common
