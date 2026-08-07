#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "common/Hash.h"
#include "storage/ReleaseStore.h"
#include "support/TemporaryDirectory.h"

namespace {

using launcher::common::sha256Hex;
using launcher::storage::ReleaseStore;
using launcher::testing::TemporaryDirectory;

const std::string ARTIFACT = "PK\x03\x04 not really a zip, but bytes are bytes";

std::filesystem::path writeArtifact(const std::filesystem::path& path, const std::string& body) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(body.data(), static_cast<std::streamsize>(body.size()));
    return path;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

TEST(ReleaseStoreTest, DerivesAFannedOutKeyEndingInZip) {
    const auto digest = sha256Hex(ARTIFACT);

    EXPECT_EQ(ReleaseStore::storageKeyFor(digest),
              digest.substr(0, 2) + "/" + digest.substr(2, 2) + "/" + digest + ".zip");
}

TEST(ReleaseStoreTest, RefusesToBuildAKeyFromSomethingThatIsNotAHash) {
    EXPECT_TRUE(ReleaseStore::storageKeyFor("").empty());
    EXPECT_TRUE(ReleaseStore::storageKeyFor("../../etc/passwd").empty());
    EXPECT_TRUE(ReleaseStore::storageKeyFor(std::string(64, 'Z')).empty());
}

TEST(ReleaseStoreTest, WritesTheArtifactAtItsContentAddress) {
    TemporaryDirectory root;
    TemporaryDirectory source;
    const ReleaseStore store(root.path());
    const auto digest = sha256Hex(ARTIFACT);

    const auto stored =
        store.store(writeArtifact(source.path() / "launcher.zip", ARTIFACT), digest);

    ASSERT_TRUE(stored.ok()) << stored.error().detail;
    EXPECT_EQ(stored.value(), ReleaseStore::storageKeyFor(digest));
    EXPECT_TRUE(store.contains(stored.value()));
    EXPECT_EQ(readFile(root.path() / stored.value()), ARTIFACT);
}

// The step that ties the signature to the file. Everything before it proves somebody signed a
// description; only this proves the file is the thing described.
TEST(ReleaseStoreTest, RefusesAnArtifactThatDoesNotMatchTheSignedHash) {
    TemporaryDirectory root;
    TemporaryDirectory source;
    const ReleaseStore store(root.path());

    const auto stored = store.store(writeArtifact(source.path() / "launcher.zip", "other bytes"),
                                    sha256Hex(ARTIFACT));

    ASSERT_FALSE(stored.ok());
    // The message carries both hashes, because the question an operator has is which file they
    // handed over.
    EXPECT_NE(stored.error().detail.find(sha256Hex("other bytes")), std::string::npos);
    EXPECT_NE(stored.error().detail.find(sha256Hex(ARTIFACT)), std::string::npos);

    // And nothing was written: a refused artifact is not a half-published one.
    EXPECT_FALSE(store.contains(ReleaseStore::storageKeyFor(sha256Hex(ARTIFACT))));
    EXPECT_FALSE(store.contains(ReleaseStore::storageKeyFor(sha256Hex("other bytes"))));
}

TEST(ReleaseStoreTest, LeavesNothingBehindInStaging) {
    TemporaryDirectory root;
    TemporaryDirectory source;
    const ReleaseStore store(root.path());

    ASSERT_TRUE(
        store.store(writeArtifact(source.path() / "a.zip", ARTIFACT), sha256Hex(ARTIFACT)).ok());

    const auto staging = root.path() / "staging";
    if (std::filesystem::exists(staging)) {
        EXPECT_TRUE(std::filesystem::is_empty(staging));
    }
}

// Re-running a publish that failed after the copy is free rather than a second copy.
TEST(ReleaseStoreTest, StoringTheSameBytesTwiceIsTheSameFile) {
    TemporaryDirectory root;
    TemporaryDirectory source;
    const ReleaseStore store(root.path());
    const auto digest = sha256Hex(ARTIFACT);
    const auto artifact = writeArtifact(source.path() / "launcher.zip", ARTIFACT);

    const auto first = store.store(artifact, digest);
    const auto second = store.store(artifact, digest);

    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(first.value(), second.value());
    EXPECT_EQ(readFile(root.path() / second.value()), ARTIFACT);
}

TEST(ReleaseStoreTest, ReportsAMissingFileRatherThanCreatingOne) {
    TemporaryDirectory root;
    const ReleaseStore store(root.path());

    const auto stored = store.store(root.path() / "nothing-here.zip", sha256Hex(ARTIFACT));

    ASSERT_FALSE(stored.ok());
    EXPECT_NE(stored.error().detail.find("no such file"), std::string::npos);
}

TEST(ReleaseStoreTest, ResolvesNothingOutsideItsRoot) {
    TemporaryDirectory root;
    const ReleaseStore store(root.path());

    EXPECT_TRUE(store.pathFor("").empty());
    EXPECT_TRUE(store.pathFor("../escaped.zip").empty());
    EXPECT_TRUE(store.pathFor("/etc/passwd").empty());
    EXPECT_FALSE(store.contains("../escaped.zip"));
}

} // namespace
