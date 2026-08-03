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
};

} // namespace launcher::repositories
