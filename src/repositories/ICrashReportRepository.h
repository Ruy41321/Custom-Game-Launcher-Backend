#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain/CrashReport.h"

namespace launcher::repositories {

inline constexpr int DEFAULT_CRASH_PAGE_SIZE = 25;
inline constexpr int MAX_CRASH_PAGE_SIZE = 100;

struct CrashQuery {
    /// Narrows to one bug. Empty lists every report, newest first.
    std::string fingerprint;
    int limit{DEFAULT_CRASH_PAGE_SIZE};
    int offset{0};
};

struct CrashPage {
    std::vector<domain::CrashReport> items;
    int64_t total{0};
};

struct CrashGroupPage {
    std::vector<domain::CrashGroup> items;
    int64_t total{0};
};

/// Persistence for crash reports. Every parameter is taken by value — a coroutine's reference
/// parameters dangle as soon as it first suspends.
///
/// There is no method taking a user id, and none returning one: a report names no account, and
/// the interface is written so that a later addition has to be a deliberate change rather than
/// an accident. See the head of migration 0004.
class ICrashReportRepository {
  public:
    virtual ~ICrashReportRepository() = default;

    virtual drogon::Task<domain::CrashReport> add(domain::NewCrashReport report,
                                                  std::string fingerprint) const = 0;

    virtual drogon::Task<CrashPage> search(CrashQuery query) const = 0;

    /// The distinct bugs, most recently seen first. This is the list an operator actually
    /// reads; the reports behind one of them are the detail.
    virtual drogon::Task<CrashGroupPage> groups(int limit, int offset) const = 0;

    virtual drogon::Task<std::optional<domain::CrashReport>> findById(std::string id) const = 0;

    /// Removes reports received longer ago than the retention window. Returns how many went.
    virtual drogon::Task<std::size_t> deleteOlderThan(uint32_t seconds) const = 0;
};

} // namespace launcher::repositories
