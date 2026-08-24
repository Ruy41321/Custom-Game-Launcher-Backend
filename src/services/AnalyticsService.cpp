#include "services/AnalyticsService.h"

#include <algorithm>

#include "domain/Role.h"

namespace launcher::services {

using common::ErrorCode;
using common::Result;

namespace permissions = launcher::domain::permissions;

AnalyticsService::AnalyticsService(const repositories::IAnalyticsRepository& analytics)
    : analytics_(analytics) {}

drogon::Task<Result<repositories::DownloadReport>>
AnalyticsService::downloadReport(domain::Actor actor, int days, int topGames) const {
    if (!actor.can(permissions::ADMIN_SETTINGS_MANAGE)) {
        co_return Result<repositories::DownloadReport>::failure(
            ErrorCode::Forbidden, "you do not have permission to do that");
    }

    // Clamped rather than refused. The window comes from a control on a page, and the two ways
    // it goes wrong — a stale bookmark and somebody typing a large number to see what happens —
    // both deserve a report rather than an error. The upper bound is what stops one request
    // walking a year of events per day for a decade.
    days = std::clamp(days, 1, repositories::MAX_ANALYTICS_DAYS);
    topGames = std::clamp(topGames, 1, repositories::MAX_ANALYTICS_TOP_GAMES);

    co_return Result<repositories::DownloadReport>::success(
        co_await analytics_.downloadReport(days, topGames));
}

} // namespace launcher::services
