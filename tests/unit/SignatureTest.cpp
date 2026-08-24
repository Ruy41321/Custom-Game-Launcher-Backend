#include <gtest/gtest.h>

#include <string>

#include "common/Signature.h"
#include "support/ReleaseSigning.h"

namespace {

using launcher::common::decodeBase64;
using launcher::common::validateP256PublicKey;
using launcher::common::verifyP256Signature;
using launcher::testing::otherReleasePrivateKey;
using launcher::testing::otherReleasePublicKey;
using launcher::testing::signWithTestKey;
using launcher::testing::testReleasePublicKey;

constexpr const char* DOCUMENT =
    R"({"schema":1,"channel":"stable","version":"0.2.0","platform":"windows","arch":"x64",)"
    R"("sha256":"0000000000000000000000000000000000000000000000000000000000000000","size":42,)"
    R"("releasedAt":"2026-08-07T10:00:00Z","notes":""})";

TEST(SignatureTest, AcceptsASignatureMadeWithTheMatchingKey) {
    const auto signature = signWithTestKey(DOCUMENT);
    ASSERT_FALSE(signature.empty());

    EXPECT_TRUE(verifyP256Signature(testReleasePublicKey(), signature, DOCUMENT).ok());
}

// The whole point of a signature, and the case a publish-time-only check would let through.
TEST(SignatureTest, RefusesASignatureMadeWithAnotherKey) {
    const auto signature = signWithTestKey(DOCUMENT, otherReleasePrivateKey());
    ASSERT_FALSE(signature.empty());

    // It is a perfectly valid signature — under a key this deployment does not trust.
    EXPECT_TRUE(verifyP256Signature(otherReleasePublicKey(), signature, DOCUMENT).ok());
    EXPECT_FALSE(verifyP256Signature(testReleasePublicKey(), signature, DOCUMENT).ok());
}

// One byte. This is the assertion that says a stored document cannot be edited in the database
// and still be served: the version, the channel and the content address are all inside it.
TEST(SignatureTest, RefusesADocumentChangedByOneCharacter) {
    const auto signature = signWithTestKey(DOCUMENT);

    std::string tampered = DOCUMENT;
    const auto version = tampered.find("0.2.0");
    ASSERT_NE(version, std::string::npos);
    tampered[version + 2] = '9';

    EXPECT_FALSE(verifyP256Signature(testReleasePublicKey(), signature, tampered).ok());
}

TEST(SignatureTest, RefusesAnEmptyOrMalformedSignature) {
    EXPECT_FALSE(verifyP256Signature(testReleasePublicKey(), "", DOCUMENT).ok());
    EXPECT_FALSE(verifyP256Signature(testReleasePublicKey(), "not base64 at all!", DOCUMENT).ok());
    // Valid base64, and not a DER signature.
    EXPECT_FALSE(verifyP256Signature(testReleasePublicKey(), "aGVsbG8=", DOCUMENT).ok());
}

TEST(SignatureTest, RefusesATruncatedSignature) {
    auto signature = signWithTestKey(DOCUMENT);
    ASSERT_GT(signature.size(), 8U);
    signature.resize(signature.size() - 8);

    EXPECT_FALSE(verifyP256Signature(testReleasePublicKey(), signature, DOCUMENT).ok());
}

TEST(SignatureTest, AcceptsAKeyWrappedAcrossLinesAsPemWrapsIt) {
    // What somebody's clipboard actually holds when they copy a PEM body out of a file.
    std::string wrapped = testReleasePublicKey();
    wrapped.insert(64, "\n");

    EXPECT_TRUE(validateP256PublicKey(wrapped).ok());
    EXPECT_TRUE(verifyP256Signature(wrapped, signWithTestKey(DOCUMENT), DOCUMENT).ok());
}

TEST(SignatureTest, RefusesAKeyThatIsNotOne) {
    EXPECT_FALSE(validateP256PublicKey("").ok());
    EXPECT_FALSE(validateP256PublicKey("not base64").ok());
    EXPECT_FALSE(validateP256PublicKey("aGVsbG8=").ok());

    // A PEM body pasted *with* its delimiter lines is the mistake somebody actually makes, so
    // the refusal names what was expected instead of only saying no.
    const std::string withArmour = std::string("-----BEGIN PUBLIC KEY-----\n") +
                                   testReleasePublicKey() + "\n-----END PUBLIC KEY-----\n";
    const auto refused = validateP256PublicKey(withArmour);
    ASSERT_FALSE(refused.ok());
    EXPECT_NE(refused.error().detail.find("BEGIN PUBLIC KEY"), std::string::npos);
}

// The algorithm is pinned rather than read out of whatever key is configured. Both of these are
// perfectly good public keys, and both would leave the client — which has one P-256 code path
// and nothing else — unable to check anything, so a launcher would stop updating for a reason
// nothing on either side reports. They are real keys rather than invented strings on purpose:
// an invented one fails to parse, which would make this test pass without ever reaching the
// check it is named after.
TEST(SignatureTest, RefusesARealKeyOfTheWrongKind) {
    // RSA-2048: parses, is a key, is not elliptic-curve.
    constexpr const char* RSA_KEY =
        "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAsRCuPzh9/1H2TXVRoocDHRMHi1ocbx2DXAlu5odH"
        "cCZH3z2EHDXOP/RAygqV6sABYu6rtyHEDxsLPLt9KRShoeg8J5bv6hnN2kVPLABMWcH+mBsejpFBK5fZTL6H"
        "UnDUWqNg7ST8r4gfJMAbLhMSzDfKSwswoQe7yF24A50UIbK5luY0x/RFpk2xNEOZZkF9Mdy2SF0D7M5Cd8be"
        "fXtsKSZJAyC8jCbLyz8PcecE403VSvKVnOp3rL/jB22+3cFPUnhtOkwPSCBQ9fZ1RzKwNuYyiN3nR2X4FEsX"
        "5vEb1gjktlFyC6IPVuB8TlLoqwpppyoB1FOZOam7iENab5i2dwIDAQAB";

    // P-384: parses, is elliptic-curve, is the wrong curve. This is the one an invented string
    // could never stand in for.
    constexpr const char* P384_KEY =
        "MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAE6C8brJzkjo7oZxgn3RM3kZ+QuxeYDkzZ+3H+g+IAaBtOuFbuZb7D"
        "bSfUas4ZUyrDKui3jmB9YGFRKAzax9UatRX11Axpyhv5dNj0lQEOCwPVl6bJNIIxgLRicAb7fe6L";

    EXPECT_FALSE(validateP256PublicKey(RSA_KEY).ok());
    EXPECT_FALSE(validateP256PublicKey(P384_KEY).ok());
}

TEST(SignatureTest, DecodesBase64AndRefusesTrailingRubbish) {
    const auto decoded = decodeBase64("aGVsbG8=");
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value(), "hello");

    // Silently decoding the prefix and stopping is how a mistyped value becomes a signature
    // that fails to verify for a reason nothing names.
    EXPECT_FALSE(decodeBase64("aGVsbG8=###").ok());
}

} // namespace
