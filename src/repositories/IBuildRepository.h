#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/Result.h"
#include "domain/Catalog.h"
#include "domain/Manifest.h"

namespace launcher::repositories {

/// Everything the manifest of a finished build asserts, applied in one shot when the upload
/// completes.
struct FinalizedManifest {
    std::vector<domain::ManifestEntry> files;
    std::string entrypointRelativePath;
    std::string defaultLaunchArgs;
    std::string manifestSha256;
    int64_t totalSizeBytes{0};
};

class IBuildRepository {
  public:
    virtual ~IBuildRepository() = default;

    /// Conflict when the version already has a build for that platform and architecture.
    virtual drogon::Task<common::Result<domain::Build>> create(domain::NewBuild build) const = 0;

    virtual drogon::Task<std::optional<domain::Build>> findById(std::string id) const = 0;

    virtual drogon::Task<std::vector<domain::Build>>
    listForVersions(std::vector<std::string> versionIds) const = 0;

    /// Resolves build → version → game in one query, so authorization never has to walk the
    /// chain itself.
    virtual drogon::Task<std::optional<domain::BuildOwnership>>
    findOwnership(std::string buildId) const = 0;

    virtual drogon::Task<std::vector<domain::ManifestEntry>>
    filesFor(std::string buildId) const = 0;

    /// Writes the file list and flips the build to `ready`, as one statement.
    ///
    /// Nullopt when the build was not still `uploading`, which is how a second, concurrent
    /// finalize is rejected: the update matches no row and the file insert, gated on it,
    /// never runs.
    virtual drogon::Task<std::optional<domain::Build>>
    finalize(std::string buildId, FinalizedManifest manifest) const = 0;

    virtual drogon::Task<bool> markFailed(std::string buildId) const = 0;

    /// Deletes a build and the manifest rows hanging off it. The blobs those rows pointed at
    /// are left alone: they may still belong to other builds, and deciding that is the
    /// collector's job, not this one's.
    virtual drogon::Task<bool> remove(std::string buildId) const = 0;
};

} // namespace launcher::repositories
