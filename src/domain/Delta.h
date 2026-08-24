#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "domain/Manifest.h"

namespace launcher::domain {

/// How a client is expected to obtain a build.
enum class DownloadKind { Full, Delta };

const char* toString(DownloadKind value);

/// One file the client does not yet have in the state the target build wants.
struct DeltaFile {
    ManifestEntry entry;
    /// A path in the client's current install that already holds exactly these bytes, or empty
    /// when they have to travel.
    ///
    /// Only ever a path the target build *keeps unchanged*, so the copy is safe whatever order
    /// the client applies the plan in: the source file is neither deleted nor overwritten by
    /// any other step of the same plan.
    std::string copyFromRelativePath;
};

/// What a client has to do to move from one build to another.
///
/// Paths are the unit of the plan, blobs are the unit of the transfer: two changed files with
/// the same content are two entries in `fetch` but one download, which is why `downloadBytes`
/// counts distinct blobs rather than summing the entries.
struct BuildDelta {
    DownloadKind kind{DownloadKind::Full};
    std::vector<DeltaFile> fetch;
    std::vector<ManifestEntry> unchanged;
    /// Paths the install holds that the target build does not have at all.
    std::vector<std::string> remove;
    /// What the transfer is expected to cost: distinct blobs in `fetch` that cannot be copied
    /// from the existing install.
    int64_t downloadBytes{0};
    /// The size of the target build as installed, which is what `downloadBytes` is judged
    /// against when deciding a delta is no longer worth it.
    int64_t totalBytes{0};
};

/// Every file of the target build, fetched from scratch. `installed` is still consulted, for
/// the paths that have to be removed — a full download replaces an install, it does not leave
/// the previous version's leftovers behind.
BuildDelta fullDownload(const std::vector<ManifestEntry>& installed,
                        const std::vector<ManifestEntry>& target);

/// The difference between what the client has and what the target build wants.
///
/// A file is unchanged when the same path holds the same content address. Anything else is
/// fetched, because the manifest is the authority on what the install must look like.
BuildDelta computeDelta(const std::vector<ManifestEntry>& installed,
                        const std::vector<ManifestEntry>& target);

} // namespace launcher::domain
