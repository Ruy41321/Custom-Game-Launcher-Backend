#pragma once

#include <string>
#include <utility>
#include <vector>

namespace launcher::domain {

/// Action keys written to the audit log.
///
/// Constants rather than literals at the call sites, because these strings are queried: an
/// operator filtering for `user.quota.changed` finds nothing if one writer spelled it
/// differently, and a typo in an audit trail is invisible until the day it matters.
namespace auditActions {
inline constexpr const char* USER_QUOTA_CHANGED = "user.quota.changed";
inline constexpr const char* USER_ACTIVATED = "user.activated";
inline constexpr const char* USER_DEACTIVATED = "user.deactivated";
inline constexpr const char* ROLE_GRANTED = "user.role.granted";
inline constexpr const char* ROLE_REVOKED = "user.role.revoked";
} // namespace auditActions

namespace auditEntities {
inline constexpr const char* USER = "user";
} // namespace auditEntities

/// One recorded administrative action.
///
/// `metadata` is carried as its JSON text rather than a parsed document: the domain layer has
/// no dependencies, and nothing above the repository needs to look inside it — the controller
/// hands it to the client and the client displays it.
struct AuditEntry {
    int64_t id{0};
    /// Empty when the account that acted has since been erased. The row survives on purpose:
    /// an audit trail that disappears with the person is not an audit trail.
    std::string actorUserId;
    std::string actorEmail;
    std::string action;
    std::string entityType;
    std::string entityId;
    std::string metadataJson{"{}"};
    std::string createdAt;
};

/// A field recorded alongside an action. Values are strings throughout: audit metadata is read
/// by a person, and a uniform shape is worth more here than preserving that a quota was a
/// number.
using AuditField = std::pair<std::string, std::string>;

struct NewAuditEntry {
    std::string actorUserId;
    std::string action;
    std::string entityType;
    std::string entityId;
    std::vector<AuditField> metadata;
};

} // namespace launcher::domain
