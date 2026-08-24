#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <string>

#include "common/Result.h"
#include "domain/Actor.h"
#include "domain/CrashReport.h"
#include "repositories/ICrashReportRepository.h"

namespace launcher::services {

struct CrashReportSettings {
    /// How long a report is kept before the sweep takes it. Thirty days by default: long
    /// enough to notice a crash that only happens on Tuesdays, short enough that a deployment
    /// is not accumulating diagnostics about a version nobody runs any more.
    uint32_t retentionSeconds{30U * 24 * 3600};
};

/// Receiving crash reports, and reading them from the operator console.
///
/// The two halves have deliberately different rules. **Submitting needs no account at all**:
/// a launcher crashes on the sign-in screen as readily as anywhere else, and a report that
/// could only be sent by somebody already signed in would be missing exactly the failures
/// worth having. **Reading needs `admin.crashes.read`**, because a stack trace is a map of the
/// program and a list of them is a list of ways to break it.
class CrashReportService {
  public:
    CrashReportService(const repositories::ICrashReportRepository& reports,
                       CrashReportSettings settings);

    /// Stores one report. Validates first — every field arrives from an unauthenticated
    /// caller — and computes the fingerprint here rather than accepting one, so no client can
    /// choose how its crashes are grouped.
    drogon::Task<common::Result<domain::CrashReport>> submit(domain::NewCrashReport report) const;

    drogon::Task<common::Result<repositories::CrashGroupPage>>
    listGroups(domain::Actor actor, int limit, int offset) const;

    drogon::Task<common::Result<repositories::CrashPage>>
    list(domain::Actor actor, repositories::CrashQuery query) const;

    drogon::Task<common::Result<domain::CrashReport>> find(domain::Actor actor,
                                                           std::string id) const;

    /// Drops what is past the retention window. Driven by the same timer machinery as the blob
    /// collector; returns how many rows went, for the log line.
    drogon::Task<std::size_t> sweepExpired() const;

  private:
    const repositories::ICrashReportRepository& reports_;
    CrashReportSettings settings_;
};

} // namespace launcher::services
