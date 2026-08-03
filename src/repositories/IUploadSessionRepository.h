#pragma once

#include <drogon/utils/coroutine.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/Result.h"

namespace launcher::repositories {

enum class UploadSessionStatus { Pending, Completed, Aborted };

const char* toDatabaseValue(UploadSessionStatus status);

struct UploadSession {
    std::string id;
    std::string buildId;
    std::string userId;
    std::string blobSha256;
    int64_t declaredSizeBytes{0};
    int64_t receivedBytes{0};
    UploadSessionStatus status{UploadSessionStatus::Pending};
    bool expired{false};

    bool complete() const { return receivedBytes >= declaredSizeBytes; }
};

struct NewUploadSession {
    std::string buildId;
    std::string userId;
    std::string blobSha256;
    int64_t declaredSizeBytes{0};
    std::chrono::seconds ttl{3600};
};

/// Bookkeeping for resumable uploads. The database, not the file on disk, is the authority on
/// how many bytes a session has accepted: the offset is handed out by a conditional update, so
/// two chunks racing at the same offset cannot both be accepted.
class IUploadSessionRepository {
  public:
    virtual ~IUploadSessionRepository() = default;

    /// Creates a session, or returns the open one that already covers this (build, blob) pair
    /// so that a retried negotiation resumes instead of starting a second staging file.
    virtual drogon::Task<common::Result<UploadSession>> create(NewUploadSession session) const = 0;

    virtual drogon::Task<std::optional<UploadSession>> findById(std::string id) const = 0;

    /// Advances the offset by `byteCount`, but only if the session is still open, unexpired
    /// and sitting at exactly `expectedOffset`. Nullopt means one of those did not hold, and
    /// the caller must not write anything.
    virtual drogon::Task<std::optional<UploadSession>>
    reserveBytes(std::string id, int64_t expectedOffset, int64_t byteCount) const = 0;

    virtual drogon::Task<bool> markCompleted(std::string id) const = 0;

    virtual drogon::Task<bool> markAborted(std::string id) const = 0;

    virtual drogon::Task<int64_t> countOpenForUser(std::string userId) const = 0;

    /// Open sessions past their expiry, oldest first. The sweeper deletes the staging file of
    /// each one before removing the row, so abandoned uploads do not hold disk forever.
    virtual drogon::Task<std::vector<UploadSession>> findExpired(int limit) const = 0;

    virtual drogon::Task<bool> deleteById(std::string id) const = 0;
};

} // namespace launcher::repositories
