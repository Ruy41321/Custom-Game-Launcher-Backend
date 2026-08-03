#pragma once

#include <drogon/utils/coroutine.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/Result.h"
#include "domain/Actor.h"
#include "domain/Catalog.h"
#include "domain/Manifest.h"
#include "repositories/IBlobRepository.h"
#include "repositories/IBuildRepository.h"
#include "repositories/IUploadSessionRepository.h"
#include "repositories/IUserRepository.h"
#include "storage/BlobStore.h"

namespace launcher::services {

struct UploadSettings {
    /// Largest single blob. A build is many files, so this bounds one file, not one build.
    int64_t maxBlobBytes{2LL * 1024 * 1024 * 1024};
    /// Largest chunk in one request. Drogon buffers a request body, so this is also what the
    /// framework's own body limit has to be raised to.
    int64_t maxChunkBytes{8LL * 1024 * 1024};
    /// How long an interrupted upload stays resumable before the sweeper reclaims its disk.
    std::chrono::seconds sessionTtl{86400};
    /// Bounds how much staging disk one account can hold while nothing is finished.
    int64_t maxOpenSessionsPerUser{16};
    /// Blobs a client may ask about in one negotiation request.
    std::size_t maxNegotiationBatch{5000};
};

struct BlobDeclaration {
    std::string sha256;
    int64_t sizeBytes{0};
};

struct FinalizeBuildCommand {
    std::vector<domain::ManifestEntry> files;
    std::string entrypointRelativePath;
    std::string defaultLaunchArgs;
};

struct ManifestDocument {
    std::string sha256;
    std::string json; ///< the exact bytes the hash covers
};

/// The upload half of publishing: which blobs the server still needs, the resumable transfer
/// of each one, and the manifest that turns a pile of blobs into a downloadable build.
///
/// Two invariants drive the design. The database, not the file on disk, decides the offset a
/// resumed upload continues at, so two chunks racing at the same offset cannot both be
/// accepted. And a blob only reaches its content address once its bytes have been hashed and
/// matched, so an interrupted or tampered transfer is discarded rather than published.
class UploadService {
  public:
    UploadService(const repositories::IBuildRepository& builds,
                  const repositories::IBlobRepository& blobs,
                  const repositories::IUploadSessionRepository& sessions,
                  const repositories::IUserRepository& users,
                  storage::BlobStore blobStore,
                  UploadSettings settings);

    /// The subset of the declared blobs the server does not already hold. This is what keeps
    /// an upload proportional to what actually changed between two builds.
    drogon::Task<common::Result<std::vector<std::string>>> missingBlobs(
        domain::Actor actor, std::string buildId, std::vector<BlobDeclaration> blobs) const;

    drogon::Task<common::Result<repositories::UploadSession>>
    beginUpload(domain::Actor actor, std::string buildId, BlobDeclaration blob) const;

    drogon::Task<common::Result<repositories::UploadSession>>
    sessionStatus(domain::Actor actor, std::string sessionId) const;

    /// Accepts one chunk at `offset`. The offset must be exactly what the session has already
    /// received; anything else is reported as a conflict carrying the real offset, which is
    /// how a client that lost track of its progress recovers.
    drogon::Task<common::Result<repositories::UploadSession>> uploadChunk(domain::Actor actor,
                                                                          std::string sessionId,
                                                                          int64_t offset,
                                                                          std::string chunk) const;

    drogon::Task<common::VoidResult> abortUpload(domain::Actor actor, std::string sessionId) const;

    drogon::Task<common::Result<domain::Build>>
    finalizeBuild(domain::Actor actor, std::string buildId, FinalizeBuildCommand command) const;

    /// The manifest of a finished build, as the exact bytes its hash covers.
    drogon::Task<common::Result<ManifestDocument>> manifest(domain::Actor actor,
                                                            std::string buildId) const;

    /// Deletes the staging files of sessions nobody came back for. Returns how many were
    /// reclaimed. Called on a timer; safe to call concurrently with live uploads because a
    /// session is only swept once it is past its expiry, at which point no chunk is accepted
    /// for it anyway.
    drogon::Task<std::size_t> sweepExpiredSessions() const;

  private:
    /// Resolves a build the actor may upload to. Ownership lives here so that no upload route
    /// can forget it.
    drogon::Task<common::Result<domain::BuildOwnership>> writableBuild(domain::Actor actor,
                                                                       std::string buildId) const;

    /// Hashes the completed staging file, moves it into the store, records the blob and
    /// charges the quota — in that order, so nothing is billed for bytes that never landed.
    drogon::Task<common::VoidResult> completeSession(repositories::UploadSession session) const;

    const repositories::IBuildRepository& builds_;
    const repositories::IBlobRepository& blobs_;
    const repositories::IUploadSessionRepository& sessions_;
    const repositories::IUserRepository& users_;
    storage::BlobStore blobStore_;
    UploadSettings settings_;
};

} // namespace launcher::services
