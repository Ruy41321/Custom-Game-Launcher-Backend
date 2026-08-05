#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace launcher::domain {

/// A registered account, as the rest of the application sees it.
///
/// Deliberately does not carry timestamps: nothing above the repository layer needs
/// `email_verified_at`, only whether verification happened, so the boolean is what the
/// domain exposes.
struct User {
    std::string id;
    std::string email;
    std::string displayName;
    std::string passwordHash;
    bool emailVerified{false};
    bool isActive{true};
    int64_t uploadQuotaBytes{0};
    int64_t uploadUsedBytes{0};

    int64_t remainingUploadBytes() const {
        const auto remaining = uploadQuotaBytes - uploadUsedBytes;
        return remaining > 0 ? remaining : 0;
    }
};

/// What the application supplies when creating an account. Separate from User because the
/// id, quota and timestamps are the database's to decide.
struct NewUser {
    std::string email;
    std::string displayName;
    std::string passwordHash;
};

// ---------------------------------------------------------------------------
// Erasure
//
// An erased account keeps its row. It has to: `games.publisher_user_id` is ON DELETE RESTRICT,
// so deleting an account that ever published would be refused by the database, and the rows
// that point at it — published games, patch notes, audit entries — are content and record that
// outlive the person. What goes is the identity the row carried.
// ---------------------------------------------------------------------------

/// What an erased account is called afterwards. Fifteen characters, which the 2–64 CHECK on
/// `users.display_name` accepts.
inline constexpr const char* ERASED_DISPLAY_NAME = "Deleted account";

/// Stored in `password_hash`, which is NOT NULL and therefore needs *something*. Deliberately
/// not a valid Argon2id encoding, so verification of any password against it fails rather than
/// depending on nobody knowing what it is.
inline constexpr const char* ERASED_PASSWORD_HASH = "erased";

/// The address the row keeps. `users.email` is `citext UNIQUE NOT NULL`, so a fixed placeholder
/// would let the second erasure on a deployment fail on the unique index; deriving it from the
/// account's own id makes it unique by construction. `.invalid` is reserved by RFC 2606, so
/// nothing can ever be delivered to it and nobody can register it.
inline std::string erasedEmailFor(std::string_view userId) {
    return "erased+" + std::string(userId) + "@deleted.invalid";
}

/// Upper bound on the free-text reason an account may give for leaving. Stored, never shown to
/// anybody but an operator.
inline constexpr std::size_t MAX_ERASURE_REASON_LENGTH = 500;

} // namespace launcher::domain
