#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace launcher::repositories {

struct BlobRecord {
    std::string sha256;
    int64_t sizeBytes{0};
    std::string storageKey;
};

/// A blob nothing points at any more, together with what has to happen when it goes: the file
/// is removed from disk, and the account that paid for those bytes gets them back.
struct CollectableBlob {
    std::string sha256;
    int64_t sizeBytes{0};
    std::string storageKey;
    /// Empty when the uploader's account has since been deleted; there is then nobody to
    /// refund, and the bytes are simply reclaimed.
    std::string uploadedByUserId;
};

class IBlobRepository {
  public:
    virtual ~IBlobRepository() = default;

    /// The subset of `sha256s` the server does not already hold, preserving the caller's
    /// order. This is what keeps an upload to the size of what actually changed.
    virtual drogon::Task<std::vector<std::string>>
    findMissing(std::vector<std::string> sha256s) const = 0;

    virtual drogon::Task<std::optional<BlobRecord>> findBySha256(std::string sha256) const = 0;

    /// The records that exist among `sha256s`, in no particular order.
    ///
    /// Used when a manifest is submitted: the sizes written into the manifest are read back
    /// from here rather than taken from the request, so a publisher cannot make a build claim
    /// a download size its blobs do not have.
    virtual drogon::Task<std::vector<BlobRecord>>
    findMany(std::vector<std::string> sha256s) const = 0;

    /// Records a newly stored blob. False when the row already existed, which tells the
    /// caller its upload was a duplicate and must not be charged against the quota.
    virtual drogon::Task<bool> record(std::string sha256,
                                      int64_t sizeBytes,
                                      std::string storageKey,
                                      std::optional<std::string> uploadedByUserId) const = 0;

    /// Blobs no manifest references and no open upload session is waiting on.
    ///
    /// `minimumAgeSeconds` is not a tuning knob but a correctness one: between the moment a
    /// blob is stored and the moment the manifest naming it is submitted, nothing references
    /// it, and a sweep with no grace period would collect the pieces of a build that is still
    /// being uploaded.
    virtual drogon::Task<std::vector<CollectableBlob>> findUnreferenced(int64_t minimumAgeSeconds,
                                                                        int limit) const = 0;

    /// Deletes a blob only if it is *still* unreferenced, and reports whether it went.
    ///
    /// The condition is repeated inside the statement rather than trusted from the listing
    /// above: a build published in between would have taken the blob, and the ON DELETE
    /// RESTRICT on build_files would turn that race into an error instead of a no-op.
    virtual drogon::Task<bool> deleteIfUnreferenced(std::string sha256,
                                                    int64_t minimumAgeSeconds) const = 0;
};

} // namespace launcher::repositories
