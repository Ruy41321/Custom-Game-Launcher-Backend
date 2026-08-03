#include "domain/Delta.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace launcher::domain {
namespace {

using PathIndex = std::unordered_map<std::string, const ManifestEntry*>;

PathIndex indexByPath(const std::vector<ManifestEntry>& entries) {
    PathIndex index;
    index.reserve(entries.size());
    for (const auto& entry : entries) {
        index.emplace(entry.relativePath, &entry);
    }
    return index;
}

/// Content the client already has *and* keeps: a path present in both manifests with the same
/// blob. Copying from one of these is safe at any point in the plan, which is the whole reason
/// the map is built from survivors rather than from everything the install happens to hold.
std::unordered_map<std::string, std::string>
survivingContent(const PathIndex& installed, const std::vector<ManifestEntry>& target) {
    std::unordered_map<std::string, std::string> byBlob;
    for (const auto& entry : target) {
        const auto found = installed.find(entry.relativePath);
        if (found == installed.end() || found->second->blobSha256 != entry.blobSha256) {
            continue;
        }
        byBlob.emplace(entry.blobSha256, entry.relativePath);
    }
    return byBlob;
}

void sortPlan(BuildDelta& delta) {
    std::sort(
        delta.fetch.begin(), delta.fetch.end(), [](const DeltaFile& lhs, const DeltaFile& rhs) {
            return lhs.entry.relativePath < rhs.entry.relativePath;
        });
    std::sort(delta.unchanged.begin(),
              delta.unchanged.end(),
              [](const ManifestEntry& lhs, const ManifestEntry& rhs) {
                  return lhs.relativePath < rhs.relativePath;
              });
    std::sort(delta.remove.begin(), delta.remove.end());
}

std::vector<std::string> pathsToRemove(const std::vector<ManifestEntry>& installed,
                                       const PathIndex& target) {
    std::vector<std::string> removed;
    for (const auto& entry : installed) {
        if (target.find(entry.relativePath) == target.end()) {
            removed.push_back(entry.relativePath);
        }
    }
    return removed;
}

/// Bytes that actually cross the network: one blob is one download however many paths it
/// lands at, and content copied from the existing install costs nothing.
int64_t transferBytes(const std::vector<DeltaFile>& fetch) {
    std::unordered_set<std::string> counted;
    int64_t bytes = 0;
    for (const auto& file : fetch) {
        if (!file.copyFromRelativePath.empty()) {
            continue;
        }
        if (counted.insert(file.entry.blobSha256).second) {
            bytes += file.entry.sizeBytes;
        }
    }
    return bytes;
}

int64_t installedSize(const std::vector<ManifestEntry>& target) {
    int64_t bytes = 0;
    for (const auto& entry : target) {
        bytes += entry.sizeBytes;
    }
    return bytes;
}

} // namespace

const char* toString(DownloadKind value) {
    switch (value) {
    case DownloadKind::Full:
        return "full";
    case DownloadKind::Delta:
        return "delta";
    }
    return "full";
}

BuildDelta fullDownload(const std::vector<ManifestEntry>& installed,
                        const std::vector<ManifestEntry>& target) {
    BuildDelta delta;
    delta.kind = DownloadKind::Full;
    delta.totalBytes = installedSize(target);

    delta.fetch.reserve(target.size());
    for (const auto& entry : target) {
        delta.fetch.push_back(DeltaFile{entry, {}});
    }
    delta.remove = pathsToRemove(installed, indexByPath(target));
    delta.downloadBytes = transferBytes(delta.fetch);

    sortPlan(delta);
    return delta;
}

BuildDelta computeDelta(const std::vector<ManifestEntry>& installed,
                        const std::vector<ManifestEntry>& target) {
    BuildDelta delta;
    delta.kind = DownloadKind::Delta;
    delta.totalBytes = installedSize(target);

    const auto installedByPath = indexByPath(installed);
    const auto reusable = survivingContent(installedByPath, target);

    for (const auto& entry : target) {
        const auto found = installedByPath.find(entry.relativePath);
        if (found != installedByPath.end() && found->second->blobSha256 == entry.blobSha256) {
            delta.unchanged.push_back(entry);
            continue;
        }

        DeltaFile file{entry, {}};
        if (const auto source = reusable.find(entry.blobSha256); source != reusable.end()) {
            file.copyFromRelativePath = source->second;
        }
        delta.fetch.push_back(std::move(file));
    }

    delta.remove = pathsToRemove(installed, indexByPath(target));
    delta.downloadBytes = transferBytes(delta.fetch);

    sortPlan(delta);
    return delta;
}

} // namespace launcher::domain
