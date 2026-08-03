#pragma once

#include <cstdint>
#include <string>

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

} // namespace launcher::domain
