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
/// An operator handed the account a one-time password. Nothing about the password itself is
/// recorded — the metadata says it happened, and the value exists in the response for as long
/// as it takes the operator to read it.
inline constexpr const char* USER_TEMPORARY_PASSWORD_SET = "user.password.temporary_set";
inline constexpr const char* ROLE_GRANTED = "user.role.granted";
inline constexpr const char* ROLE_REVOKED = "user.role.revoked";
/// Written by the account itself, so the actor and the entity are the same id. The row stays
/// readable afterwards because the account is anonymised rather than deleted — an audit trail
/// that vanished with the person would be exactly the trail nobody could rely on.
inline constexpr const char* USER_ERASED = "user.erased";
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
    /// Empty when nothing acted — a grant from the command line. It is *not* empty for an
    /// account that has since been erased: erasure anonymises the row rather than deleting it,
    /// so the `ON DELETE SET NULL` never fires and the entries of one operator stay linked to
    /// each other. An audit trail that came apart with the person would not be one.
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
