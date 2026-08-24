#pragma once

#include <drogon/utils/coroutine.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "common/Random.h"
#include "repositories/ICrashReportRepository.h"

namespace launcher::testing {

/// In-memory double for the crash-report store.
///
/// Keeps the reports in arrival order and groups them by fingerprint exactly as the statement
/// does, because grouping is the behaviour the service is worth testing for — a fake that
/// returned one row per report would let a bug through unnoticed.
class FakeCrashReportRepository : public repositories::ICrashReportRepository {
  public:
    mutable std::vector<domain::CrashReport> reports;
    /// Set by tests so `deleteOlderThan` has something to delete; the fake keeps no clock.
    mutable std::vector<std::string> expiredIds;

    drogon::Task<domain::CrashReport> add(domain::NewCrashReport report,
                                          std::string fingerprint) const override {
        domain::CrashReport stored;
        stored.id = common::randomUuid();
        stored.kind = report.kind;
        stored.occurredAt = report.occurredAt;
        stored.launcherVersion = report.launcherVersion;
        stored.platform = report.platform;
        stored.exceptionType = report.exceptionType;
        stored.message = report.message;
        stored.stackTrace = report.stackTrace;
        stored.fingerprint = std::move(fingerprint);
        stored.receivedAt = "2026-08-05T12:00:00Z";
        reports.push_back(stored);
        co_return stored;
    }

    drogon::Task<repositories::CrashPage> search(repositories::CrashQuery query) const override {
        std::vector<domain::CrashReport> matched;
        for (const auto& report : reports) {
            if (query.fingerprint.empty() || report.fingerprint == query.fingerprint) {
                matched.push_back(report);
            }
        }

        repositories::CrashPage page;
        page.total = static_cast<int64_t>(matched.size());

        const auto begin =
            std::min<std::size_t>(static_cast<std::size_t>(query.offset), matched.size());
        const auto end =
            std::min<std::size_t>(begin + static_cast<std::size_t>(query.limit), matched.size());
        page.items.assign(matched.begin() + static_cast<std::ptrdiff_t>(begin),
                          matched.begin() + static_cast<std::ptrdiff_t>(end));
        co_return page;
    }

    drogon::Task<repositories::CrashGroupPage> groups(int limit, int offset) const override {
        std::vector<std::string> order;
        std::map<std::string, domain::CrashGroup> grouped;

        for (const auto& report : reports) {
            auto found = grouped.find(report.fingerprint);
            if (found == grouped.end()) {
                domain::CrashGroup group;
                group.fingerprint = report.fingerprint;
                group.firstSeenAt = report.receivedAt;
                order.push_back(report.fingerprint);
                found = grouped.emplace(report.fingerprint, std::move(group)).first;
            }

            // The newest report wins the summary, as DISTINCT ON does in the real statement.
            found->second.exceptionType = report.exceptionType;
            found->second.message = report.message;
            found->second.latestReportId = report.id;
            found->second.lastSeenAt = report.receivedAt;
            ++found->second.occurrences;
        }

        repositories::CrashGroupPage page;
        page.total = static_cast<int64_t>(order.size());

        const auto begin = std::min<std::size_t>(static_cast<std::size_t>(offset), order.size());
        const auto end =
            std::min<std::size_t>(begin + static_cast<std::size_t>(limit), order.size());
        for (auto index = begin; index < end; ++index) {
            page.items.push_back(grouped[order[index]]);
        }
        co_return page;
    }

    drogon::Task<std::optional<domain::CrashReport>> findById(std::string id) const override {
        for (const auto& report : reports) {
            if (report.id == id) {
                co_return report;
            }
        }
        co_return std::nullopt;
    }

    drogon::Task<std::size_t> deleteOlderThan(uint32_t) const override {
        const auto before = reports.size();
        reports.erase(std::remove_if(reports.begin(),
                                     reports.end(),
                                     [&](const auto& report) {
                                         return std::find(expiredIds.begin(),
                                                          expiredIds.end(),
                                                          report.id) != expiredIds.end();
                                     }),
                      reports.end());
        co_return before - reports.size();
    }
};

} // namespace launcher::testing
