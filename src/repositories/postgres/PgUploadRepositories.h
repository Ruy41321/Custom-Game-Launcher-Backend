#pragma once

#include <drogon/orm/DbClient.h>

#include "repositories/IBlobRepository.h"
#include "repositories/IUploadSessionRepository.h"

namespace launcher::repositories::postgres {

/// PostgreSQL implementations for content-addressed storage and resumable uploads.
/// See PgSupport.h for the conventions every repository here follows.

class PgBlobRepository : public IBlobRepository {
  public:
    explicit PgBlobRepository(drogon::orm::DbClientPtr database);

    drogon::Task<std::vector<std::string>>
    findMissing(std::vector<std::string> sha256s) const override;

    drogon::Task<std::optional<BlobRecord>> findBySha256(std::string sha256) const override;

    drogon::Task<std::vector<BlobRecord>> findMany(std::vector<std::string> sha256s) const override;

    drogon::Task<bool> record(std::string sha256,
                              int64_t sizeBytes,
                              std::string storageKey,
                              std::optional<std::string> uploadedByUserId) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

class PgUploadSessionRepository : public IUploadSessionRepository {
  public:
    explicit PgUploadSessionRepository(drogon::orm::DbClientPtr database);

    drogon::Task<common::Result<UploadSession>> create(NewUploadSession session) const override;

    drogon::Task<std::optional<UploadSession>> findById(std::string id) const override;

    drogon::Task<std::optional<UploadSession>>
    reserveBytes(std::string id, int64_t expectedOffset, int64_t byteCount) const override;

    drogon::Task<bool> markCompleted(std::string id) const override;

    drogon::Task<bool> markAborted(std::string id) const override;

    drogon::Task<int64_t> countOpenForUser(std::string userId) const override;

    drogon::Task<std::vector<UploadSession>> findExpired(int limit) const override;

    drogon::Task<bool> deleteById(std::string id) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

} // namespace launcher::repositories::postgres
