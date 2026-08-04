#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "common/Hash.h"
#include "domain/Media.h"
#include "storage/MediaStore.h"
#include "support/TemporaryDirectory.h"

namespace {

using launcher::common::sha256Hex;
using launcher::domain::ImageFormat;
using launcher::storage::MediaStore;
using launcher::testing::TemporaryDirectory;

const std::string PNG_BODY = std::string("\x89PNG\r\n\x1a\n", 8) + "a picture";

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

TEST(MediaStoreTest, DerivesAFannedOutKeyThatCarriesTheExtension) {
    const auto digest = sha256Hex(PNG_BODY);

    const auto key = MediaStore::storageKeyFor(digest, ImageFormat::Png);

    // The extension is not decoration: nginx answers with a content type from its own mime
    // table, and a hashed name with no suffix is served as application/octet-stream.
    EXPECT_EQ(key, digest.substr(0, 2) + "/" + digest.substr(2, 2) + "/" + digest + ".png");
}

TEST(MediaStoreTest, RefusesToBuildAKeyFromSomethingThatIsNotAHash) {
    EXPECT_TRUE(MediaStore::storageKeyFor("", ImageFormat::Png).empty());
    EXPECT_TRUE(MediaStore::storageKeyFor("../../etc/passwd", ImageFormat::Png).empty());
    EXPECT_TRUE(MediaStore::storageKeyFor(std::string(64, 'Z'), ImageFormat::Png).empty());
}

TEST(MediaStoreTest, WritesTheImageAtItsContentAddress) {
    TemporaryDirectory root;
    const MediaStore store(root.path());

    const auto stored = store.store(PNG_BODY, ImageFormat::Png);

    ASSERT_TRUE(stored.ok());
    const auto key = stored.value();
    EXPECT_EQ(key, MediaStore::storageKeyFor(sha256Hex(PNG_BODY), ImageFormat::Png));
    EXPECT_TRUE(store.contains(key));
    EXPECT_EQ(readFile(root.path() / key), PNG_BODY);
}

TEST(MediaStoreTest, LeavesNothingBehindInStaging) {
    TemporaryDirectory root;
    const MediaStore store(root.path());

    ASSERT_TRUE(store.store(PNG_BODY, ImageFormat::Png).ok());

    const auto staging = root.path() / "staging";
    if (std::filesystem::exists(staging)) {
        EXPECT_TRUE(std::filesystem::is_empty(staging));
    }
}

TEST(MediaStoreTest, StoringTheSameImageTwiceWritesOneFile) {
    TemporaryDirectory root;
    const MediaStore store(root.path());

    const auto first = store.store(PNG_BODY, ImageFormat::Png);
    const auto second = store.store(PNG_BODY, ImageFormat::Png);

    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    // Identical content addresses are identical content: two games sharing a cover share the
    // file, which is the whole point of storing artwork this way.
    EXPECT_EQ(first.value(), second.value());
    EXPECT_EQ(readFile(root.path() / first.value()), PNG_BODY);
}

TEST(MediaStoreTest, DifferentBytesTakeDifferentAddresses) {
    TemporaryDirectory root;
    const MediaStore store(root.path());

    const auto first = store.store(PNG_BODY, ImageFormat::Png);
    const auto second = store.store(PNG_BODY + " again", ImageFormat::Png);

    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_NE(first.value(), second.value());
}

TEST(MediaStoreTest, RemovesAStoredImage) {
    TemporaryDirectory root;
    const MediaStore store(root.path());
    const auto stored = store.store(PNG_BODY, ImageFormat::Png);
    ASSERT_TRUE(stored.ok());

    store.remove(stored.value());

    EXPECT_FALSE(store.contains(stored.value()));
}

TEST(MediaStoreTest, RemovingSomethingThatIsNotThereIsSilent) {
    TemporaryDirectory root;
    const MediaStore store(root.path());

    // A sweep that races another one finds the file already gone, and that is the ordinary
    // outcome rather than an error.
    EXPECT_NO_THROW(store.remove(MediaStore::storageKeyFor(sha256Hex("absent"), ImageFormat::Png)));
}

TEST(MediaStoreTest, RefusesToResolveAKeyThatWouldEscapeTheRoot) {
    TemporaryDirectory root;
    const MediaStore store(root.path());

    // Keys are written by this class, but they are read back out of the database, and a row
    // that somehow held a traversing key must resolve to nothing rather than to a real file.
    EXPECT_TRUE(store.pathFor("../../etc/passwd").empty());
    EXPECT_TRUE(store.pathFor("/etc/passwd").empty());
    EXPECT_TRUE(store.pathFor("").empty());
    EXPECT_FALSE(store.contains("../../etc/passwd"));
}

} // namespace
