#include "services/RetentionService.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace launcher::services {

RetentionService::RetentionService(const repositories::IBlobRepository& blobs,
                                   const repositories::IUserRepository& users,
                                   storage::BlobStore store,
                                   RetentionSettings settings)
    : blobs_(blobs),
      users_(users),
      store_(std::move(store)),
      settings_(settings) {}

drogon::Task<RetentionService::SweepReport> RetentionService::collectUnreferencedBlobs() const {
    SweepReport report;

    const auto candidates =
        co_await blobs_.findUnreferenced(settings_.blobGraceSeconds, settings_.batchSize);

    for (const auto& blob : candidates) {
        // The row goes first, and the file second. The other order is the one that cannot be
        // recovered from: if the file were removed and the delete then lost a race against a
        // build that had just taken the blob, a live manifest would point at nothing and every
        // download of it would fail. This way the worst a crash between the two steps leaves
        // is a file nothing references, which costs disk and nothing else.
        const bool deleted =
            co_await blobs_.deleteIfUnreferenced(blob.sha256, settings_.blobGraceSeconds);
        if (!deleted) {
            ++report.skipped;
            continue;
        }

        store_.discard(store_.pathFor(blob.sha256));

        // Quota is charged when an upload completes, so without this it is a lifetime cap:
        // deleting a build would free the disk and leave the publisher still paying for it.
        if (!blob.uploadedByUserId.empty()) {
            co_await users_.releaseUpload(blob.uploadedByUserId, blob.sizeBytes);
        }

        ++report.collected;
        report.reclaimedBytes += blob.sizeBytes;
    }

    if (report.collected > 0 || report.skipped > 0) {
        spdlog::info("blob sweep collected {} blobs ({} bytes), skipped {} still referenced",
                     report.collected,
                     report.reclaimedBytes,
                     report.skipped);
    }
    co_return report;
}

} // namespace launcher::services
