#pragma once

#include <drogon/utils/coroutine.h>

#include <cstddef>
#include <cstdint>

#include "repositories/IBlobRepository.h"
#include "repositories/IUserRepository.h"
#include "storage/BlobStore.h"

namespace launcher::services {

struct RetentionSettings {
    /// How long a blob must have existed before it can be collected.
    ///
    /// Not a tuning knob: a publisher uploads every blob of a build *before* submitting the
    /// manifest that names them, so during a publish there is a window in which perfectly live
    /// content is referenced by nothing at all. The grace period has to be longer than the
    /// slowest publish a deployment expects, which is why it defaults to the same day the
    /// upload sessions themselves are given.
    int64_t blobGraceSeconds{86400};

    /// How many blobs one pass may collect. A sweep that tried to reclaim a terabyte in one
    /// go would hold the event loop for the whole of it; the next pass picks up the rest.
    int batchSize{500};
};

/// Reclaims storage nothing references any more.
///
/// `build_files.blob_sha256` is ON DELETE RESTRICT, so a referenced blob has never been at
/// risk — the gap this closes is the other half: until now nothing removed the *unreferenced*
/// ones either, so deleting a build freed no disk and no quota, and an upload that was never
/// finalised was paid for forever.
class RetentionService {
  public:
    RetentionService(const repositories::IBlobRepository& blobs,
                     const repositories::IUserRepository& users,
                     storage::BlobStore store,
                     RetentionSettings settings);

    struct SweepReport {
        std::size_t collected{0};
        int64_t reclaimedBytes{0};
        /// Blobs that were listed as collectable and had been taken by a build by the time the
        /// delete ran. Not an error: it is the race working as intended.
        std::size_t skipped{0};
    };

    drogon::Task<SweepReport> collectUnreferencedBlobs() const;

  private:
    const repositories::IBlobRepository& blobs_;
    const repositories::IUserRepository& users_;
    storage::BlobStore store_;
    RetentionSettings settings_;
};

} // namespace launcher::services
