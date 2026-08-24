#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <string>
#include <vector>

namespace launcher::repositories {

inline constexpr int DEFAULT_ANALYTICS_DAYS = 30;
inline constexpr int MAX_ANALYTICS_DAYS = 365;
inline constexpr int DEFAULT_ANALYTICS_TOP_GAMES = 10;
inline constexpr int MAX_ANALYTICS_TOP_GAMES = 50;

struct DownloadTotals {
    int64_t downloads{0};
    /// Distinct accounts, which is the closest thing to "how many people" this schema can
    /// answer: `download_events.user_id` goes null when an account is erased, and those rows
    /// stay in the count of downloads but leave the count of people.
    int64_t distinctUsers{0};
    /// What the plans expected to transfer, not what clients actually pulled. The file server
    /// answers the transfer itself and never reports back, so this is an upper bound — a client
    /// that cancelled halfway is counted in full.
    int64_t bytesPlanned{0};
    int64_t fullDownloads{0};
    int64_t deltaDownloads{0};
};

struct DownloadDay {
    std::string day; ///< YYYY-MM-DD, UTC
    int64_t downloads{0};
    int64_t bytesPlanned{0};
};

struct GameDownloads {
    std::string gameId;
    std::string slug;
    std::string title;
    int64_t downloads{0};
    int64_t bytesPlanned{0};
    int64_t distinctUsers{0};
};

/// One page of numbers, because the console shows one page. Assembling it here rather than
/// exposing three endpoints keeps the window consistent across the three panels: a caller
/// cannot end up with totals from one range and a chart from another.
struct DownloadReport {
    int days{DEFAULT_ANALYTICS_DAYS};
    DownloadTotals totals;
    /// One entry per day in the window, including the days nothing happened — a chart that
    /// silently skipped empty days would draw a busier picture than the truth.
    std::vector<DownloadDay> daily;
    std::vector<GameDownloads> topGames;
};

/// Reads over `download_events`, which the download planner has been writing since the delta
/// work and nothing has ever read.
class IAnalyticsRepository {
  public:
    virtual ~IAnalyticsRepository() = default;

    virtual drogon::Task<DownloadReport> downloadReport(int days, int topGames) const = 0;
};

} // namespace launcher::repositories
