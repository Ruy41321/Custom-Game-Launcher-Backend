#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace launcher::common {

/// Application-level failure categories. Each maps to exactly one HTTP status so that
/// services can signal intent without knowing anything about HTTP.
enum class ErrorCode {
    InvalidInput,
    Unauthenticated,
    Forbidden,
    /// A valid session on an account holding a password somebody else chose. Its own category
    /// rather than a `Forbidden` a client would have to tell apart from every other refusal by
    /// its prose: there is exactly one thing to do about it, and the category is what says so.
    PasswordChangeRequired,
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

    /// The stable name of the *specific* rule that refused, where `code` names only the
    /// category every refusal of that kind shares. Empty when a failure has no rule worth
    /// naming, which is the default and stays valid.
    ///
    /// It exists because `detail` is English prose written for whoever reads the logs, and it
    /// is the only thing a client could otherwise act on. Matching on prose makes rewording a
    /// message a breaking change, so the wording stays free to improve and this stays fixed.
    /// A client maps it to a sentence in its own language and falls back to the category for
    /// a rule it does not know, which is what makes adding one a non-breaking change.
    std::string rule;

    /// The values the rule's sentence needs, in order — almost always the limit that was
    /// exceeded. Without them a translated message can say a password is too short but not
    /// what "long enough" is, which is the half the person typing can act on.
    std::vector<std::string> ruleArgs;

    Error() = default;

    Error(ErrorCode errorCode, std::string errorDetail)
        : code(errorCode),
          detail(std::move(errorDetail)) {}

    Error(ErrorCode errorCode,
          std::string errorDetail,
          std::string ruleName,
          std::vector<std::string> ruleArguments = {})
        : code(errorCode),
          detail(std::move(errorDetail)),
          rule(std::move(ruleName)),
          ruleArgs(std::move(ruleArguments)) {}
};

inline Error invalidInput(std::string detail) {
    return {ErrorCode::InvalidInput, std::move(detail)};
}

inline Error
invalidInput(std::string detail, std::string rule, std::vector<std::string> args = {}) {
    return {ErrorCode::InvalidInput, std::move(detail), std::move(rule), std::move(args)};
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
