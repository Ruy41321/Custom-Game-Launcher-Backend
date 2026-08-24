#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "domain/Delta.h"

namespace {

using launcher::domain::BuildDelta;
using launcher::domain::computeDelta;
using launcher::domain::DownloadKind;
using launcher::domain::fullDownload;
using launcher::domain::ManifestEntry;

/// A recognisable stand-in for a content address. The delta never parses one, so what matters
/// is only that different content reads differently in a failure message.
std::string blob(char marker) {
    return std::string(64, marker);
}

ManifestEntry file(std::string path, char content, int64_t size = 100) {
    ManifestEntry entry;
    entry.relativePath = std::move(path);
    entry.blobSha256 = blob(content);
    entry.sizeBytes = size;
    return entry;
}

std::vector<std::string> fetchedPaths(const BuildDelta& delta) {
    std::vector<std::string> paths;
    for (const auto& fetched : delta.fetch) {
        paths.push_back(fetched.entry.relativePath);
    }
    return paths;
}

std::string copySourceFor(const BuildDelta& delta, const std::string& path) {
    for (const auto& fetched : delta.fetch) {
        if (fetched.entry.relativePath == path) {
            return fetched.copyFromRelativePath;
        }
    }
    return "<not fetched>";
}

// ---------------------------------------------------------------------------
// Full downloads
// ---------------------------------------------------------------------------

TEST(DeltaTest, AFirstInstallFetchesEverything) {
    const std::vector<ManifestEntry> target{file("Game.exe", 'a', 10), file("data/pak", 'b', 90)};

    const auto delta = fullDownload({}, target);

    EXPECT_EQ(delta.kind, DownloadKind::Full);
    EXPECT_EQ(fetchedPaths(delta), (std::vector<std::string>{"Game.exe", "data/pak"}));
    EXPECT_TRUE(delta.unchanged.empty());
    EXPECT_TRUE(delta.remove.empty());
    EXPECT_EQ(delta.downloadBytes, 100);
    EXPECT_EQ(delta.totalBytes, 100);
}

// A full download replaces an install rather than adding to it, so what the previous version
// left behind still has to go.
TEST(DeltaTest, AFullDownloadStillRemovesWhatTheTargetDoesNotHave) {
    const std::vector<ManifestEntry> installed{file("Game.exe", 'a'), file("old.dll", 'z')};
    const std::vector<ManifestEntry> target{file("Game.exe", 'b')};

    const auto delta = fullDownload(installed, target);

    EXPECT_EQ(delta.remove, (std::vector<std::string>{"old.dll"}));
    EXPECT_EQ(fetchedPaths(delta), (std::vector<std::string>{"Game.exe"}));
}

// ---------------------------------------------------------------------------
// Deltas
// ---------------------------------------------------------------------------

TEST(DeltaTest, FetchesOnlyWhatChangedBetweenTwoBuilds) {
    const std::vector<ManifestEntry> installed{
        file("Game.exe", 'a', 10), file("data/pak", 'b', 500), file("gone.dll", 'z', 5)};
    const std::vector<ManifestEntry> target{
        file("Game.exe", 'c', 12), file("data/pak", 'b', 500), file("new.dll", 'd', 7)};

    const auto delta = computeDelta(installed, target);

    EXPECT_EQ(delta.kind, DownloadKind::Delta);
    EXPECT_EQ(fetchedPaths(delta), (std::vector<std::string>{"Game.exe", "new.dll"}));
    ASSERT_EQ(delta.unchanged.size(), 1U);
    EXPECT_EQ(delta.unchanged[0].relativePath, "data/pak");
    EXPECT_EQ(delta.remove, (std::vector<std::string>{"gone.dll"}));
    EXPECT_EQ(delta.downloadBytes, 19) << "only the changed files travel";
    EXPECT_EQ(delta.totalBytes, 519) << "the size of the build as installed";
}

// The same file at a new path is one download, not two: blobs are the unit of transfer even
// though paths are the unit of the plan.
TEST(DeltaTest, CountsContentSharedByTwoNewPathsOnce) {
    const std::vector<ManifestEntry> target{file("a.dll", 'x', 40), file("plugins/a.dll", 'x', 40)};

    const auto delta = computeDelta({file("Game.exe", 'q', 1)}, target);

    EXPECT_EQ(fetchedPaths(delta).size(), 2U);
    EXPECT_EQ(delta.downloadBytes, 40);
    EXPECT_EQ(delta.totalBytes, 80);
}

TEST(DeltaTest, AnIdenticalBuildNeedsNothing) {
    const std::vector<ManifestEntry> files{file("Game.exe", 'a'), file("data/pak", 'b')};

    const auto delta = computeDelta(files, files);

    EXPECT_TRUE(delta.fetch.empty());
    EXPECT_TRUE(delta.remove.empty());
    EXPECT_EQ(delta.unchanged.size(), 2U);
    EXPECT_EQ(delta.downloadBytes, 0);
}

// ---------------------------------------------------------------------------
// Copying instead of downloading
// ---------------------------------------------------------------------------

// A file that merely moved is already on the client's disk. Copying it is only offered from a
// path the target build keeps unchanged, so the source cannot be deleted or overwritten by
// another step of the same plan.
TEST(DeltaTest, OffersALocalCopyWhenTheContentSurvivesAtAnotherPath) {
    const std::vector<ManifestEntry> installed{file("engine.dll", 'e', 300)};
    const std::vector<ManifestEntry> target{file("engine.dll", 'e', 300),
                                            file("bin/engine.dll", 'e', 300)};

    const auto delta = computeDelta(installed, target);

    EXPECT_EQ(fetchedPaths(delta), (std::vector<std::string>{"bin/engine.dll"}));
    EXPECT_EQ(copySourceFor(delta, "bin/engine.dll"), "engine.dll");
    EXPECT_EQ(delta.downloadBytes, 0) << "content already on disk does not travel";
}

// The dangerous case: the content exists locally, but only at a path this same plan replaces.
// Copying from it would depend on the client's ordering, so it is never offered.
TEST(DeltaTest, DoesNotOfferACopyFromAPathTheUpdateOverwrites) {
    const std::vector<ManifestEntry> installed{file("engine.dll", 'e', 300)};
    const std::vector<ManifestEntry> target{file("engine.dll", 'f', 310),
                                            file("bin/engine.dll", 'e', 300)};

    const auto delta = computeDelta(installed, target);

    EXPECT_EQ(copySourceFor(delta, "bin/engine.dll"), "");
    EXPECT_EQ(delta.downloadBytes, 610);
}

TEST(DeltaTest, DoesNotOfferACopyFromAPathTheUpdateDeletes) {
    const std::vector<ManifestEntry> installed{file("engine.dll", 'e', 300)};
    const std::vector<ManifestEntry> target{file("bin/engine.dll", 'e', 300)};

    const auto delta = computeDelta(installed, target);

    EXPECT_EQ(copySourceFor(delta, "bin/engine.dll"), "");
    EXPECT_EQ(delta.remove, (std::vector<std::string>{"engine.dll"}));
    EXPECT_EQ(delta.downloadBytes, 300);
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

TEST(DeltaTest, OrdersEveryListByPathWhateverOrderTheManifestsArriveIn) {
    const std::vector<ManifestEntry> installed{file("z.dll", 'z'), file("a.dll", 'a')};
    const std::vector<ManifestEntry> target{file("m.dll", 'm'), file("b.dll", 'b')};

    const auto delta = computeDelta(installed, target);

    EXPECT_EQ(fetchedPaths(delta), (std::vector<std::string>{"b.dll", "m.dll"}));
    EXPECT_EQ(delta.remove, (std::vector<std::string>{"a.dll", "z.dll"}));
}

} // namespace
