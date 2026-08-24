#include <gtest/gtest.h>

#include <drogon/utils/coroutine.h>

#include <fstream>
#include <string>

#include "common/Hash.h"
#include "common/Random.h"
#include "services/RetentionService.h"
#include "support/FakeRepositories.h"
#include "support/FakeUploadRepositories.h"
#include "support/TemporaryDirectory.h"

namespace {

using launcher::services::RetentionService;
using launcher::services::RetentionSettings;
using launcher::storage::BlobStore;
using launcher::testing::FakeBlobRepository;
using launcher::testing::FakeUserRepository;
using launcher::testing::TemporaryDirectory;

constexpr int64_t GRACE_SECONDS = 3600;

/// Keeps the fakes and the store alive for the whole test.
struct RetentionFixture {
    RetentionFixture()
        : service(blobs, users, BlobStore{root.path()}, settings()) {}

    static RetentionSettings settings() {
        RetentionSettings retention;
        retention.blobGraceSeconds = GRACE_SECONDS;
        retention.batchSize = 100;
        return retention;
    }

    TemporaryDirectory root;
    FakeBlobRepository blobs;
    FakeUserRepository users;
    RetentionService service;

    std::string seedUser(int64_t used) const {
        launcher::domain::User user;
        user.email = "publisher@example.test";
        user.uploadQuotaBytes = 1000000;
        user.uploadUsedBytes = used;
        return users.seed(user).id;
    }

    /// Writes a blob at its content address and records it, with an age and an owner.
    std::string
    seedBlob(const std::string& contents, const std::string& uploaderId, int64_t ageSeconds) const {
        const auto digest = launcher::common::sha256Hex(contents);
        const BlobStore store(root.path());
        const auto path = store.pathFor(digest);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file << contents;
        file.close();

        blobs.records.push_back(launcher::repositories::BlobRecord{
            digest, static_cast<int64_t>(contents.size()), BlobStore::storageKeyFor(digest)});
        blobs.uploaders.push_back(uploaderId);
        blobs.ageSecondsBySha[digest] = ageSeconds;
        return digest;
    }

    bool fileExists(const std::string& sha256) const {
        return BlobStore{root.path()}.contains(sha256);
    }

    int64_t usedBytesOf(const std::string& userId) const {
        for (const auto& user : users.users) {
            if (user.id == userId) {
                return user.uploadUsedBytes;
            }
        }
        return -1;
    }
};

TEST(RetentionServiceTest, CollectsABlobNoManifestReferences) {
    RetentionFixture fixture;
    const auto owner = fixture.seedUser(64);
    const auto blob = fixture.seedBlob("orphaned content", owner, GRACE_SECONDS + 1);

    const auto report = drogon::sync_wait(fixture.service.collectUnreferencedBlobs());

    EXPECT_EQ(report.collected, 1u);
    EXPECT_EQ(report.reclaimedBytes, static_cast<int64_t>(std::string("orphaned content").size()));
    EXPECT_TRUE(fixture.blobs.records.empty());
    EXPECT_FALSE(fixture.fileExists(blob));
}

TEST(RetentionServiceTest, GivesTheUploaderTheirQuotaBack) {
    RetentionFixture fixture;
    const auto owner = fixture.seedUser(500);
    const std::string contents = "sixteen bytes!!!";
    fixture.seedBlob(contents, owner, GRACE_SECONDS + 1);

    drogon::sync_wait(fixture.service.collectUnreferencedBlobs());

    // Without this the quota is a lifetime cap: deleting a build would free the disk and leave
    // the publisher still paying for it.
    EXPECT_EQ(fixture.usedBytesOf(owner), 500 - static_cast<int64_t>(contents.size()));
}

TEST(RetentionServiceTest, LeavesABlobAManifestStillReferences) {
    RetentionFixture fixture;
    const auto owner = fixture.seedUser(64);
    const auto blob = fixture.seedBlob("live content", owner, GRACE_SECONDS + 1);
    fixture.blobs.referenced.push_back(blob);

    const auto report = drogon::sync_wait(fixture.service.collectUnreferencedBlobs());

    EXPECT_EQ(report.collected, 0u);
    EXPECT_EQ(fixture.blobs.records.size(), 1u);
    EXPECT_TRUE(fixture.fileExists(blob));
    EXPECT_EQ(fixture.usedBytesOf(owner), 64) << "nothing was collected, so nothing is refunded";
}

TEST(RetentionServiceTest, LeavesABlobThatIsYoungerThanTheGracePeriod) {
    RetentionFixture fixture;
    const auto owner = fixture.seedUser(64);
    const auto blob = fixture.seedBlob("just uploaded", owner, GRACE_SECONDS - 1);

    const auto report = drogon::sync_wait(fixture.service.collectUnreferencedBlobs());

    // A publisher uploads every blob of a build before submitting the manifest that names
    // them, so during a publish live content is referenced by nothing. Without the grace
    // period the collector eats builds that are still being uploaded.
    EXPECT_EQ(report.collected, 0u);
    EXPECT_TRUE(fixture.fileExists(blob));
}

TEST(RetentionServiceTest, CollectsABlobWhoseUploaderHasGoneWithoutRefundingAnybody) {
    RetentionFixture fixture;
    const auto blob = fixture.seedBlob("ownerless", "", GRACE_SECONDS + 1);

    const auto report = drogon::sync_wait(fixture.service.collectUnreferencedBlobs());

    EXPECT_EQ(report.collected, 1u);
    EXPECT_FALSE(fixture.fileExists(blob));
}

TEST(RetentionServiceTest, TheDeleteRepeatsTheConditionSoALateReferenceStillWins) {
    RetentionFixture fixture;
    const auto owner = fixture.seedUser(64);
    const auto blob = fixture.seedBlob("contended", owner, GRACE_SECONDS + 1);

    // This is the state the sweep can find itself in: the blob was collectable when it was
    // listed, and a build published in between took it. The delete asks again rather than
    // trusting the listing, so the race ends in a no-op — and it has to, because build_files
    // is ON DELETE RESTRICT and the database would otherwise turn it into an error.
    const auto candidates = drogon::sync_wait(fixture.blobs.findUnreferenced(GRACE_SECONDS, 100));
    ASSERT_EQ(candidates.size(), 1u);
    fixture.blobs.referenced.push_back(blob);

    const bool deleted = drogon::sync_wait(fixture.blobs.deleteIfUnreferenced(blob, GRACE_SECONDS));

    EXPECT_FALSE(deleted);
    EXPECT_EQ(fixture.blobs.records.size(), 1u);
}

TEST(RetentionServiceTest, CollectsNoMoreThanOneBatch) {
    RetentionFixture fixture;
    const auto owner = fixture.seedUser(1000);
    for (int index = 0; index < 5; ++index) {
        fixture.seedBlob("orphan " + std::to_string(index), owner, GRACE_SECONDS + 1);
    }

    RetentionSettings small;
    small.blobGraceSeconds = GRACE_SECONDS;
    small.batchSize = 2;
    const RetentionService limited(
        fixture.blobs, fixture.users, BlobStore{fixture.root.path()}, small);

    const auto report = drogon::sync_wait(limited.collectUnreferencedBlobs());

    // A sweep that tried to reclaim everything at once would hold an event loop for the whole
    // of it; the next pass picks up the rest.
    EXPECT_EQ(report.collected, 2u);
    EXPECT_EQ(fixture.blobs.records.size(), 3u);
}

TEST(RetentionServiceTest, ASweepWithNothingToDoIsSilent) {
    RetentionFixture fixture;

    const auto report = drogon::sync_wait(fixture.service.collectUnreferencedBlobs());

    EXPECT_EQ(report.collected, 0u);
    EXPECT_EQ(report.skipped, 0u);
    EXPECT_EQ(report.reclaimedBytes, 0);
}

} // namespace
