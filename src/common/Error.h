#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace launcher::common {

/// Application-level failure categories. Each maps to exactly one HTTP status so that
/// services can signal intent without knowing anything about HTTP.
enum class ErrorCode {
    InvalidInput,
    Unauthenticated,
    Forbidden,
    NotFound,
    Conflict,
    QuotaExceeded,
    RateLimited,
    DependencyFailure,
    Internal,
};

int httpStatusFor(ErrorCode code);

const char* titleFor(ErrorCode code);

const char* nameFor(ErrorCode code);

struct Error {
    ErrorCode code{ErrorCode::Internal};
    std::string detail;

    Error() = default;

    Error(ErrorCode errorCode, std::string errorDetail)
        : code(errorCode),
          detail(std::move(errorDetail)) {}
};

inline Error invalidInput(std::string detail) {
    return {ErrorCode::InvalidInput, std::move(detail)};
}

inline Error notFound(std::string detail) {
    return {ErrorCode::NotFound, std::move(detail)};
}

inline Error conflict(std::string detail) {
    return {ErrorCode::Conflict, std::move(detail)};
}

inline Error internalError(std::string detail) {
    return {ErrorCode::Internal, std::move(detail)};
}

/// Thrown for failures that cannot be handled locally. Caught by the single central
/// exception handler, which turns it into the standard error envelope.
class ApiException : public std::runtime_error {
  public:
    explicit ApiException(Error error)
        : std::runtime_error(error.detail),
          error_(std::move(error)) {}

    ApiException(ErrorCode code, std::string detail)
        : ApiException(Error{code, std::move(detail)}) {}

    const Error& error() const noexcept { return error_; }

  private:
    Error error_;
};

} // namespace launcher::common
