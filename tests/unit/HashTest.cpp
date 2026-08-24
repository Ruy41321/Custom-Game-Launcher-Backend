#include <gtest/gtest.h>

#include <string>

#include "common/Hash.h"
#include "support/TemporaryDirectory.h"

namespace {

using launcher::common::constantTimeEquals;
using launcher::common::sha256File;
using launcher::common::sha256Hex;
using launcher::testing::TemporaryDirectory;

// Well-known FIPS 180-2 vectors: if these ever change, the CAS layout is broken.
constexpr const char* EMPTY_SHA256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr const char* ABC_SHA256 =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

TEST(HashTest, MatchesKnownSha256Vectors) {
    EXPECT_EQ(sha256Hex(""), EMPTY_SHA256);
    EXPECT_EQ(sha256Hex("abc"), ABC_SHA256);
}

TEST(HashTest, ProducesLowercaseHexOfTheRightLength) {
    const auto digest = sha256Hex("some build file contents");

    EXPECT_EQ(digest.size(), 64U);
    EXPECT_EQ(digest.find_first_not_of("0123456789abcdef"), std::string::npos);
}

TEST(HashTest, HandlesEmbeddedNulBytes) {
    const std::string withNul("a\0b", 3);

    EXPECT_NE(sha256Hex(withNul), sha256Hex("ab"));
    EXPECT_NE(sha256Hex(withNul), sha256Hex("a"));
}

TEST(HashTest, FileHashMatchesTheInMemoryHash) {
    const TemporaryDirectory directory("launcher-hash");

    // Larger than the streaming chunk so the multi-read path is exercised.
    const std::string contents(3 * 1024 * 1024 + 17, 'x');
    const auto file = directory.writeFile("blob.bin", contents);

    const auto result = sha256File(file);

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_EQ(result.value(), sha256Hex(contents));
}

TEST(HashTest, FileHashOfAnEmptyFileMatchesTheEmptyDigest) {
    const TemporaryDirectory directory("launcher-hash");
    const auto file = directory.writeFile("empty.bin", "");

    const auto result = sha256File(file);

    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_EQ(result.value(), EMPTY_SHA256);
}

TEST(HashTest, FileHashOfAMissingFileFails) {
    const TemporaryDirectory directory("launcher-hash");

    const auto result = sha256File(directory.path() / "does-not-exist");

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, launcher::common::ErrorCode::NotFound);
}

TEST(HashTest, ConstantTimeEqualsBehavesLikeEquality) {
    EXPECT_TRUE(constantTimeEquals("secret", "secret"));
    EXPECT_TRUE(constantTimeEquals("", ""));
    EXPECT_FALSE(constantTimeEquals("secret", "secrEt"));
    EXPECT_FALSE(constantTimeEquals("secret", "secret "));
    EXPECT_FALSE(constantTimeEquals("secret", ""));
}

} // namespace
