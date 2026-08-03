#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/Random.h"
#include "repositories/IBlobRepository.h"
#include "repositories/IUploadSessionRepository.h"

namespace launcher::testing {

/// In-memory doubles for content-addressed storage and upload sessions.
///
/// They model the behaviour the service relies on — deduplication by content address, and an
/// offset that only advances when it matches — because that is what the tests are about.
///
/// The interface methods are const because callers only read through them, so the recorded
/// state is mutable.

class FakeBlobRepository : public repositories::IBlobRepository {
  public:
    mutable std::vector<repositories::BlobRecord> records;
    mutable std::vector<std::string> uploaders;

    void seed(const std::string& sha256, int64_t sizeBytes) const {
        records.push_back(repositories::BlobRecord{sha256, sizeBytes, sha256});
    }

    drogon::Task<std::vector<std::string>>
    findMissing(std::vector<std::string> sha256s) const override {
        std::vector<std::string> missing;
        for (const auto& sha256 : sha256s) {
            const bool present = std::any_of(
                records.begin(), records.end(), [&](const repositories::BlobRecord& record) {
                    return record.sha256 == sha256;
                });
            if (!present) {
                missing.push_back(sha256);
            }
        }
        co_return missing;
    }

    drogon::Task<std::optional<repositories::BlobRecord>>
    findBySha256(std::string sha256) const override {
        for (const auto& record : records) {
            if (record.sha256 == sha256) {
                co_return record;
            }
        }
        co_return std::nullopt;
    }

    drogon::Task<std::vector<repositories::BlobRecord>>
    findMany(std::vector<std::string> sha256s) const override {
        std::vector<repositories::BlobRecord> matched;
        for (const auto& record : records) {
            if (std::find(sha256s.begin(), sha256s.end(), record.sha256) != sha256s.end()) {
                matched.push_back(record);
            }
        }
        co_return matched;
    }

    drogon::Task<bool> record(std::string sha256,
                              int64_t sizeBytes,
                              std::string storageKey,
                              std::optional<std::string> uploadedByUserId) const override {
        if (co_await findBySha256(sha256)) {
            co_return false;
        }
        records.push_back(repositories::BlobRecord{sha256, sizeBytes, storageKey});
        uploaders.push_back(uploadedByUserId.value_or(""));
        co_return true;
    }
};

class FakeUploadSessionRepository : public repositories::IUploadSessionRepository {
  public:
    mutable std::vector<repositories::UploadSession> sessions;
    /// Tests flip this to model a session whose expiry has passed.
    mutable std::unordered_set<std::string> expiredIds;

    repositories::UploadSession* find(const std::string& id) const {
        for (auto& session : sessions) {
            if (session.id == id) {
                session.expired = expiredIds.count(id) > 0;
                return &session;
            }
        }
        return nullptr;
    }

    drogon::Task<common::Result<repositories::UploadSession>>
    create(repositories::NewUploadSession request) const override {
        for (const auto& session : sessions) {
            if (session.buildId == request.buildId && session.blobSha256 == request.blobSha256 &&
                session.status == repositories::UploadSessionStatus::Pending) {
                co_return common::Result<repositories::UploadSession>::success(session);
            }
        }

        repositories::UploadSession created;
        created.id = common::randomUuid();
        created.buildId = request.buildId;
        created.userId = request.userId;
        created.blobSha256 = request.blobSha256;
        created.declaredSizeBytes = request.declaredSizeBytes;
        sessions.push_back(created);
        co_return common::Result<repositories::UploadSession>::success(created);
    }

    drogon::Task<std::optional<repositories::UploadSession>>
    findById(std::string id) const override {
        if (const auto* session = find(id)) {
            co_return *session;
        }
        co_return std::nullopt;
    }

    drogon::Task<std::optional<repositories::UploadSession>>
    reserveBytes(std::string id, int64_t expectedOffset, int64_t byteCount) const override {
        auto* session = find(id);
        if (session == nullptr || session->status != repositories::UploadSessionStatus::Pending ||
            session->expired || session->receivedBytes != expectedOffset ||
            session->receivedBytes + byteCount > session->declaredSizeBytes) {
            co_return std::nullopt;
        }
        session->receivedBytes += byteCount;
        co_return *session;
    }

    drogon::Task<bool> markCompleted(std::string id) const override {
        auto* session = find(id);
        if (session == nullptr || session->status != repositories::UploadSessionStatus::Pending) {
            co_return false;
        }
        session->status = repositories::UploadSessionStatus::Completed;
        co_return true;
    }

    drogon::Task<bool> markAborted(std::string id) const override {
        auto* session = find(id);
        if (session == nullptr || session->status != repositories::UploadSessionStatus::Pending) {
            co_return false;
        }
        session->status = repositories::UploadSessionStatus::Aborted;
        co_return true;
    }

    drogon::Task<int64_t> countOpenForUser(std::string userId) const override {
        int64_t open = 0;
        for (const auto& session : sessions) {
            if (session.userId == userId &&
                session.status == repositories::UploadSessionStatus::Pending &&
                expiredIds.count(session.id) == 0) {
                ++open;
            }
        }
        co_return open;
    }

    drogon::Task<std::vector<repositories::UploadSession>> findExpired(int limit) const override {
        std::vector<repositories::UploadSession> expired;
        for (const auto& session : sessions) {
            if (static_cast<int>(expired.size()) >= limit) {
                break;
            }
            if (session.status == repositories::UploadSessionStatus::Pending &&
                expiredIds.count(session.id) > 0) {
                expired.push_back(session);
            }
        }
        co_return expired;
    }

    drogon::Task<bool> deleteById(std::string id) const override {
        const auto before = sessions.size();
        sessions.erase(std::remove_if(sessions.begin(),
                                      sessions.end(),
                                      [&](const repositories::UploadSession& session) {
                                          return session.id == id;
                                      }),
                       sessions.end());
        co_return sessions.size() != before;
    }
};

} // namespace launcher::testing
