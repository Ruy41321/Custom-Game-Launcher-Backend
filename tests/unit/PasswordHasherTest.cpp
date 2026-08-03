#include <gtest/gtest.h>

#include <set>
#include <string>

#include "common/Random.h"
#include "services/PasswordHasher.h"

namespace {

using launcher::services::Argon2idPasswordHasher;
using launcher::services::PasswordHashingSettings;

// The cheapest parameters libsodium accepts, so the suite stays fast. Production uses the
// defaults; what is under test here is behaviour, not cost.
PasswordHashingSettings fastSettings() {
    return PasswordHashingSettings{2, 64U * 1024U * 1024U};
}

TEST(PasswordHasherTest, HashesAndVerifiesTheSamePassword) {
    const Argon2idPasswordHasher hasher(fastSettings());

    const auto hash = hasher.hash("correct horse battery staple");

    ASSERT_TRUE(hash.ok()) << hash.error().detail;
    EXPECT_TRUE(hasher.verify("correct horse battery staple", hash.value()));
}

TEST(PasswordHasherTest, RejectsAWrongPassword) {
    const Argon2idPasswordHasher hasher(fastSettings());
    const auto hash = hasher.hash("correct horse battery staple");
    ASSERT_TRUE(hash.ok());

    EXPECT_FALSE(hasher.verify("Correct horse battery staple", hash.value()));
    EXPECT_FALSE(hasher.verify("", hash.value()));
    EXPECT_FALSE(hasher.verify("correct horse battery stapl", hash.value()));
}

TEST(PasswordHasherTest, ProducesAnArgon2idHash) {
    const Argon2idPasswordHasher hasher(fastSettings());

    const auto hash = hasher.hash("correct horse battery staple");

    ASSERT_TRUE(hash.ok());
    // The encoded form carries its own algorithm, salt and parameters.
    EXPECT_EQ(hash.value().rfind("$argon2id$", 0), 0U) << hash.value();
}

// A shared salt would let one rainbow table cover every account.
TEST(PasswordHasherTest, SaltsEveryHashDifferently) {
    const Argon2idPasswordHasher hasher(fastSettings());

    std::set<std::string> hashes;
    for (int i = 0; i < 3; ++i) {
        const auto hash = hasher.hash("the same password every time");
        ASSERT_TRUE(hash.ok());
        hashes.insert(hash.value());
    }

    EXPECT_EQ(hashes.size(), 3U);
}

// A corrupt or truncated row must fail closed, never throw and never authenticate.
TEST(PasswordHasherTest, TreatsAMalformedStoredHashAsAFailedVerification) {
    const Argon2idPasswordHasher hasher(fastSettings());

    for (const auto* stored : {"", "not-a-hash", "$argon2id$truncated", "$2y$10$bcryptlookalike"}) {
        EXPECT_FALSE(hasher.verify("correct horse battery staple", stored)) << stored;
    }
}

TEST(PasswordHasherTest, DoesNotAskForARehashWithMatchingParameters) {
    const Argon2idPasswordHasher hasher(fastSettings());
    const auto hash = hasher.hash("correct horse battery staple");
    ASSERT_TRUE(hash.ok());

    EXPECT_FALSE(hasher.needsRehash(hash.value()));
}

// Raising the cost must transparently upgrade existing hashes on next login.
TEST(PasswordHasherTest, AsksForARehashWhenParametersGetStronger) {
    const Argon2idPasswordHasher weak(PasswordHashingSettings{2, 64U * 1024U * 1024U});
    const auto hash = weak.hash("correct horse battery staple");
    ASSERT_TRUE(hash.ok());

    const Argon2idPasswordHasher strong(PasswordHashingSettings{4, 128U * 1024U * 1024U});

    EXPECT_TRUE(strong.needsRehash(hash.value()));
    // The old hash must still verify, or every user would be locked out by the upgrade.
    EXPECT_TRUE(strong.verify("correct horse battery staple", hash.value()));
}

TEST(PasswordHasherTest, AsksForARehashOfAnUnreadableHash) {
    const Argon2idPasswordHasher hasher(fastSettings());

    EXPECT_TRUE(hasher.needsRehash(""));
    EXPECT_TRUE(hasher.needsRehash("garbage"));
}

TEST(RandomTest, ProducesDistinctUrlSafeTokens) {
    std::set<std::string> tokens;
    for (int i = 0; i < 100; ++i) {
        const auto token = launcher::common::randomUrlSafeToken();
        EXPECT_FALSE(token.empty());
        EXPECT_EQ(token.find_first_not_of(
                      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"),
                  std::string::npos)
            << token;
        tokens.insert(token);
    }

    EXPECT_EQ(tokens.size(), 100U);
}

TEST(RandomTest, ProducesWellFormedVersion4Uuids) {
    std::set<std::string> uuids;
    for (int i = 0; i < 100; ++i) {
        const auto uuid = launcher::common::randomUuid();
        ASSERT_EQ(uuid.size(), 36U) << uuid;
        EXPECT_EQ(uuid[8], '-');
        EXPECT_EQ(uuid[13], '-');
        EXPECT_EQ(uuid[18], '-');
        EXPECT_EQ(uuid[23], '-');
        EXPECT_EQ(uuid[14], '4') << "not a version 4 uuid: " << uuid;
        EXPECT_NE(std::string("89ab").find(uuid[19]), std::string::npos)
            << "wrong variant nibble: " << uuid;
        uuids.insert(uuid);
    }

    EXPECT_EQ(uuids.size(), 100U);
}

} // namespace
