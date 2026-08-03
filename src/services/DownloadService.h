#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/Result.h"
#include "domain/Actor.h"
#include "domain/Catalog.h"
#include "domain/Delta.h"
#include "domain/Manifest.h"
#include "repositories/IBuildRepository.h"
#include "repositories/IDownloadRepository.h"
#include "storage/DownloadUrlSigner.h"

namespace launcher::services {

struct DownloadSettings {
    /// Past this ratio of transferred bytes to installed bytes, a delta stops being worth its
    /// bookkeeping and the server plans a full download instead. See CLAUDE.md §3.
    double fullDownloadThresholdRatio{0.7};
};

/// One file of a plan, with the URL that fetches its content.
struct DownloadFile {
    domain::ManifestEntry entry;
    /// A path in the current install already holding these bytes, or empty. A hint: the URL is
    /// filled in either way, so a client that cannot find the local copy is never stuck.
    std::string copyFromRelativePath;
    std::string url;
};

/// Everything a client needs to reach a build's exact contents, and nothing it has to compute
/// for itself.
struct DownloadPlan {
    std::string buildId;
    std::string gameId;
    std::string gameVersionId;
    domain::DownloadKind kind{domain::DownloadKind::Full};
    std::string manifestSha256;
    std::string entrypointRelativePath;
    std::string defaultLaunchArgs;

    std::vector<DownloadFile> files;
    std::vector<domain::ManifestEntry> unchanged;
    std::vector<std::string> remove;

    int64_t downloadBytes{0};
    int64_t totalBytes{0};
    int64_t urlsExpireAt{0}; ///< seconds since the epoch
};

/// What a client says it currently has on disk, one entry per file it found.
struct InstalledFile {
    std::string relativePath;
    std::string sha256;
};

struct IntegrityReport {
    std::string buildId;
    std::string manifestSha256;
    bool intact{true};

    std::vector<std::string> missing;    ///< in the manifest, absent from the install
    std::vector<std::string> corrupt;    ///< present, but not the content the manifest names
    std::vector<std::string> unexpected; ///< in the install, absent from the manifest

    /// The files to fetch again to make the install match the manifest.
    std::vector<DownloadFile> repair;
    int64_t repairBytes{0};
    int64_t urlsExpireAt{0};
};

/// The download half of a build's life: what a client still needs, where to get it, and
/// whether what it ended up with is actually the build.
///
/// The API never serves blob bytes. It answers with signed URLs the file server validates on
/// its own, so a multi-gigabyte transfer never occupies an API worker and `Range` — the basis
/// of a resumed download — is handled by nginx natively.
class DownloadService {
  public:
    DownloadService(const repositories::IBuildRepository& builds,
                    const repositories::IDownloadRepository& downloads,
                    storage::DownloadUrlSigner signer,
                    DownloadSettings settings);

    /// What it takes to go from `fromBuildId` (empty for a first install) to `buildId`.
    ///
    /// The delta is computed against the two manifests on demand, so a client on any old build
    /// reaches the current one in a single step rather than replaying every release since.
    drogon::Task<common::Result<DownloadPlan>>
    plan(domain::Actor actor, std::string buildId, std::string fromBuildId) const;

    /// Compares what the client found on disk with what the build asserts, and hands back the
    /// URLs that repair the difference. The manifest is the authority: an install that
    /// disagrees with it is wrong, however it got that way.
    drogon::Task<common::Result<IntegrityReport>> verifyInstall(
        domain::Actor actor, std::string buildId, std::vector<InstalledFile> installed) const;

  private:
    /// Resolves a build the actor may download, or the reason they may not. Every rule lives
    /// here so no download route can forget one.
    drogon::Task<common::Result<domain::BuildOwnership>>
    downloadableBuild(domain::Actor actor, std::string buildId) const;

    std::vector<DownloadFile> signAll(const std::vector<domain::DeltaFile>& files,
                                      int64_t expiresAt) const;

    const repositories::IBuildRepository& builds_;
    const repositories::IDownloadRepository& downloads_;
    storage::DownloadUrlSigner signer_;
    DownloadSettings settings_;
};

} // namespace launcher::services
