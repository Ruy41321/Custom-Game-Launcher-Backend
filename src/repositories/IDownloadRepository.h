#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <string>

#include "domain/Delta.h"

namespace launcher::repositories {

/// One client being handed a download plan. Recorded for the publisher's own statistics, which
/// is why it keeps the version the client was coming *from*: how many installs are stuck on an
/// old build is exactly the question a small publisher wants answered.
struct DownloadEvent {
    std::string gameId;
    std::string buildId;
    std::string userId;
    /// The version the client is updating from, empty for a first install.
    std::string fromVersionId;
    domain::DownloadKind kind{domain::DownloadKind::Full};
    /// What the plan expected to transfer, not what the client eventually pulled: the file
    /// server answers the transfer itself and never reports back.
    int64_t bytesPlanned{0};
};

class IDownloadRepository {
  public:
    virtual ~IDownloadRepository() = default;

    virtual drogon::Task<bool> recordEvent(DownloadEvent event) const = 0;
};

} // namespace launcher::repositories
