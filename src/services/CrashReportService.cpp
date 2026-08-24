#include "services/CrashReportService.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

#include "common/Logging.h"
#include "domain/Role.h"
#include "domain/Validation.h"

namespace launcher::services {
namespace {

using common::ErrorCode;
using common::Result;
using common::VoidResult;

namespace permissions = launcher::domain::permissions;

VoidResult requireReader(const domain::Actor& actor) {
    if (!actor.can(permissions::ADMIN_CRASHES_READ)) {
        return VoidResult::failure(ErrorCode::Forbidden, "you cannot read crash reports");
    }
    return VoidResult::success();
}

domain::NewCrashReport trimmed(domain::NewCrashReport report) {
    report.kind = domain::trim(report.kind);
    report.occurredAt = domain::trim(report.occurredAt);
    report.launcherVersion = domain::trim(report.launcherVersion);
    report.platform = domain::trim(report.platform);
    report.exceptionType = domain::trim(report.exceptionType);
    return report;
}

} // namespace

CrashReportService::CrashReportService(const repositories::ICrashReportRepository& reports,
                                       CrashReportSettings settings)
    : reports_(reports),
      settings_(settings) {}

drogon::Task<Result<domain::CrashReport>>
CrashReportService::submit(domain::NewCrashReport report) const {
    auto candidate = trimmed(std::move(report));

    if (auto check = domain::validateCrashReport(candidate); !check.ok()) {
        co_return Result<domain::CrashReport>::failure(check.error());
    }

    // Computed here and never accepted from the caller: it decides what counts as one bug, and
    // a client that chose its own could hide a crash among a thousand distinct ones.
    auto fingerprint = domain::crashFingerprint(candidate);

    auto stored = co_await reports_.add(std::move(candidate), fingerprint);

    // The fingerprint and nothing else. This line is written for every report an anonymous
    // caller sends, and putting a message in it would copy whatever they sent into the log.
    spdlog::info("crash report received fingerprint={}", common::escapeJson(stored.fingerprint));
    co_return Result<domain::CrashReport>::success(std::move(stored));
}

drogon::Task<Result<repositories::CrashGroupPage>>
CrashReportService::listGroups(domain::Actor actor, int limit, int offset) const {
    using Page = Result<repositories::CrashGroupPage>;

    if (auto allowed = requireReader(actor); !allowed.ok()) {
        co_return Page::failure(allowed.error());
    }

    limit = std::clamp(limit, 1, repositories::MAX_CRASH_PAGE_SIZE);
    offset = std::max(offset, 0);

    co_return Page::success(co_await reports_.groups(limit, offset));
}

drogon::Task<Result<repositories::CrashPage>>
CrashReportService::list(domain::Actor actor, repositories::CrashQuery query) const {
    using Page = Result<repositories::CrashPage>;

    if (auto allowed = requireReader(actor); !allowed.ok()) {
        co_return Page::failure(allowed.error());
    }

    query.limit = std::clamp(query.limit, 1, repositories::MAX_CRASH_PAGE_SIZE);
    query.offset = std::max(query.offset, 0);

    co_return Page::success(co_await reports_.search(std::move(query)));
}

drogon::Task<Result<domain::CrashReport>> CrashReportService::find(domain::Actor actor,
                                                                   std::string id) const {
    using Found = Result<domain::CrashReport>;

    if (auto allowed = requireReader(actor); !allowed.ok()) {
        co_return Found::failure(allowed.error());
    }
    // Guarded before the value can reach a `$1::uuid` comparison, which PostgreSQL answers with
    // an error rather than an empty result.
    if (!domain::isUuid(id)) {
        co_return Found::failure(ErrorCode::NotFound, "no such crash report");
    }

    auto found = co_await reports_.findById(std::move(id));
    if (!found.has_value()) {
        co_return Found::failure(ErrorCode::NotFound, "no such crash report");
    }
    co_return Found::success(std::move(*found));
}

drogon::Task<std::size_t> CrashReportService::sweepExpired() const {
    const auto removed = co_await reports_.deleteOlderThan(settings_.retentionSeconds);
    if (removed > 0) {
        spdlog::info("crash report sweep removed {} reports past retention", removed);
    }
    co_return removed;
}

} // namespace launcher::services
