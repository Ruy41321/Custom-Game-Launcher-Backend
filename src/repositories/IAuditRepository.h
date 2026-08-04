#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <string>
#include <vector>

#include "domain/AuditEntry.h"

namespace launcher::repositories {

inline constexpr int DEFAULT_AUDIT_PAGE_SIZE = 50;
inline constexpr int MAX_AUDIT_PAGE_SIZE = 200;

struct AuditQuery {
    /// A blank field is an absent filter, not a filter for a blank value.
    std::string actorUserId;
    std::string action;
    std::string entityType;
    std::string entityId;
    int limit{DEFAULT_AUDIT_PAGE_SIZE};
    int offset{0};
};

struct AuditPage {
    std::vector<domain::AuditEntry> items;
    int64_t total{0};
};

/// Reading the audit trail.
///
/// Read-only by design, and there is no `record` here on purpose: an entry is written by the
/// same statement as the action it describes, so that an action can never exist without its
/// row. Appending afterwards would mean a failed insert leaves a change nobody can attribute,
/// which is the one outcome an obligatory audit trail has to rule out.
class IAuditRepository {
  public:
    virtual ~IAuditRepository() = default;

    virtual drogon::Task<AuditPage> search(AuditQuery query) const = 0;
};

} // namespace launcher::repositories
