#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "common/Hash.h"
#include "storage/BlobStore.h"
#include "support/TemporaryDirectory.h"

namespace {

using launcher::common::ErrorCode;
using launcher::common::sha256Hex;
using launcher::storage::BlobStore;
using launcher::testing::TemporaryDirectory;

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

TEST(BlobStoreTest, DerivesATwoLevelStorageKeyFromTheHash) {
    const auto digest = sha256Hex("hello");

    const auto key = BlobStore::storageKeyFor(digest);

    EXPECT_EQ(key, digest.substr(0, 2) + "/" + digest.substr(2, 2) + "/" + digest);
}

// The key is a filesystem path built from client-supplied data, so anything that is not a
// content address must not produce a path at all.
TEST(BlobStoreTest, RefusesToBuildAPathFromSomethingThatIsNotAHash) {
    EXPECT_TRUE(BlobStore::storageKeyFor("../../etc/passwd").empty());
    EXPECT_TRUE(BlobStore::storageKeyFor("").empty());
    EXPECT_TRUE(BlobStore::storageKeyFor(std::string(64, 'A')).empty());

    TemporaryDirectory root;
    const BlobStore store(root.path());
    EXPECT_TRUE(store.pathFor("not-a-hash").empty());
    EXPECT_FALSE(store.contains("not-a-hash"));
}

TEST(BlobStoreTest, WritesChunksAtTheirOffsetAndCommitsWhenTheHashMatches) {
    TemporaryDirectory root;
    const BlobStore store(root.path());

    const std::string content = "the quick brown fox";
    const auto digest = sha256Hex(content);
    const auto staging = store.stagingPathFor("11111111-1111-1111-1111-111111111111");

    ASSERT_TRUE(store.writeAt(staging, 0, content.substr(0, 4)).ok());
    ASSERT_TRUE(store.writeAt(staging, 4, content.substr(4)).ok());
    EXPECT_EQ(store.sizeOf(staging), static_cast<int64_t>(content.size()));

    ASSERT_TRUE(store.commit(staging, digest).ok());

    EXPECT_TRUE(store.contains(digest));
    EXPECT_EQ(readFile(store.pathFor(digest)), content);
    EXPECT_FALSE(std::filesystem::exists(staging)) << "the staging file must not survive a commit";
}

// The single most important rule of the upload path: bytes that do not add up to the address
// they claim never reach the store.
TEST(BlobStoreTest, DiscardsAnUploadWhoseBytesDoNotMatchTheDeclaredHash) {
    TemporaryDirectory root;
    const BlobStore store(root.path());

    const auto claimed = sha256Hex("what the client promised");
    const auto staging = store.stagingPathFor("22222222-2222-2222-2222-222222222222");
    ASSERT_TRUE(store.writeAt(staging, 0, "what the client actually sent").ok());

    const auto committed = store.commit(staging, claimed);

    ASSERT_FALSE(committed.ok());
    EXPECT_EQ(committed.error().code, ErrorCode::InvalidInput);
    EXPECT_FALSE(store.contains(claimed));
    EXPECT_FALSE(std::filesystem::exists(staging));
}

// Deduplication is the whole point of content addressing: the second upload of identical
// content is a no-op, not a conflict.
TEST(BlobStoreTest, TreatsAnAlreadyStoredBlobAsSuccess) {
    TemporaryDirectory root;
    const BlobStore store(root.path());

    const std::string content = "shared between two builds";
    const auto digest = sha256Hex(content);

    const auto first = store.stagingPathFor("33333333-3333-3333-3333-333333333333");
    ASSERT_TRUE(store.writeAt(first, 0, content).ok());
    ASSERT_TRUE(store.commit(first, digest).ok());

    const auto second = store.stagingPathFor("44444444-4444-4444-4444-444444444444");
    ASSERT_TRUE(store.writeAt(second, 0, content).ok());

    EXPECT_TRUE(store.commit(second, digest).ok());
    EXPECT_EQ(readFile(store.pathFor(digest)), content);
    EXPECT_FALSE(std::filesystem::exists(second));
}

TEST(BlobStoreTest, RefusesToStartAnUploadPastTheBeginning) {
    TemporaryDirectory root;
    const BlobStore store(root.path());
    const auto staging = store.stagingPathFor("55555555-5555-5555-5555-555555555555");

    const auto written = store.writeAt(staging, 128, "late");

    ASSERT_FALSE(written.ok());
    EXPECT_EQ(written.error().code, ErrorCode::Conflict);
}

// A rewritten range has to land where it was written, or resuming after a lost response would
// append the same bytes twice.
TEST(BlobStoreTest, OverwritesRatherThanAppendsWhenAChunkIsRepeated) {
    TemporaryDirectory root;
    const BlobStore store(root.path());
    const auto staging = store.stagingPathFor("66666666-6666-6666-6666-666666666666");

    ASSERT_TRUE(store.writeAt(staging, 0, "abcdef").ok());
    ASSERT_TRUE(store.writeAt(staging, 3, "XYZ").ok());

    EXPECT_EQ(readFile(staging), "abcXYZ");
    EXPECT_EQ(store.sizeOf(staging), 6);
}

TEST(BlobStoreTest, DiscardIsSilentForAFileThatIsNotThere) {
    TemporaryDirectory root;
    const BlobStore store(root.path());

    EXPECT_NO_THROW(store.discard(store.stagingPathFor("77777777-7777-7777-7777-777777777777")));
    EXPECT_EQ(store.sizeOf(root.path() / "missing"), 0);
}

} // namespace
