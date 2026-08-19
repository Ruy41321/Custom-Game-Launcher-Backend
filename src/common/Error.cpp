#include "common/Error.h"

namespace launcher::common {

int httpStatusFor(ErrorCode code) {
    switch (code) {
    case ErrorCode::InvalidInput:
        return 422;
    case ErrorCode::Unauthenticated:
        return 401;
    case ErrorCode::Forbidden:
    case ErrorCode::PasswordChangeRequired:
        return 403;
    case ErrorCode::NotFound:
        return 404;
    case ErrorCode::Conflict:
        return 409;
    case ErrorCode::QuotaExceeded:
        return 413;
    case ErrorCode::RateLimited:
        return 429;
    case ErrorCode::DependencyFailure:
        return 503;
    case ErrorCode::Internal:
        break;
    }
    return 500;
}

const char* titleFor(ErrorCode code) {
    switch (code) {
    case ErrorCode::InvalidInput:
        return "Validation failed";
    case ErrorCode::Unauthenticated:
        return "Authentication required";
    case ErrorCode::Forbidden:
        return "Forbidden";
    case ErrorCode::PasswordChangeRequired:
        return "Password change required";
    case ErrorCode::NotFound:
        return "Not found";
    case ErrorCode::Conflict:
        return "Conflict";
    case ErrorCode::QuotaExceeded:
        return "Quota exceeded";
    case ErrorCode::RateLimited:
        return "Too many requests";
    case ErrorCode::DependencyFailure:
        return "Service unavailable";
    case ErrorCode::Internal:
        break;
    }
    return "Internal server error";
}

const char* nameFor(ErrorCode code) {
    switch (code) {
    case ErrorCode::InvalidInput:
        return "invalid_input";
    case ErrorCode::Unauthenticated:
        return "unauthenticated";
    case ErrorCode::Forbidden:
        return "forbidden";
    case ErrorCode::PasswordChangeRequired:
        return "password_change_required";
    case ErrorCode::NotFound:
        return "not_found";
    case ErrorCode::Conflict:
        return "conflict";
    case ErrorCode::QuotaExceeded:
        return "quota_exceeded";
    case ErrorCode::RateLimited:
        return "rate_limited";
    case ErrorCode::DependencyFailure:
        return "dependency_failure";
    case ErrorCode::Internal:
        break;
    }
    return "internal";
}

} // namespace launcher::common
