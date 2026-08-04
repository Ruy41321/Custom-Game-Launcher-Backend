#pragma once

#include <drogon/utils/coroutine.h>

#include "common/Result.h"
#include "domain/Actor.h"
#include "repositories/IAnalyticsRepository.h"

namespace launcher::services {

/// Download statistics for the operator console.
///
/// Server-wide rather than per publisher, which is why it takes the operator-wide permission
/// rather than a per-game one: the question it answers is "what is this deployment doing",
/// and the answer spans every publisher on it. A publisher's own view of their own game is a
/// different surface, on the public API, and does not exist yet.
class AnalyticsService {
  public:
    explicit AnalyticsService(const repositories::IAnalyticsRepository& analytics);

    drogon::Task<common::Result<repositories::DownloadReport>>
    downloadReport(domain::Actor actor, int days, int topGames) const;

  private:
    const repositories::IAnalyticsRepository& analytics_;
};

} // namespace launcher::services
