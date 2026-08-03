#include "domain/Validation.h"

#include <algorithm>
#include <cctype>

namespace launcher::domain {
namespace {

using common::ErrorCode;
using common::VoidResult;

bool isWhitespace(unsigned char c) {
    return std::isspace(c) != 0;
}

bool containsControlCharacter(std::string_view text) {
    return std::any_of(
        text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; });
}

} // namespace

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && isWhitespace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }

    std::size_t end = text.size();
    while (end > begin && isWhitespace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return std::string(text.substr(begin, end - begin));
}

std::string normalizeEmail(std::string_view email) {
    std::string normalized = trim(email);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

VoidResult validateEmail(std::string_view email) {
    const std::string candidate = trim(email);

    if (candidate.empty()) {
        return VoidResult::failure(ErrorCode::InvalidInput, "email is required");
    }
    if (candidate.size() > MAX_EMAIL_LENGTH) {
        return VoidResult::failure(ErrorCode::InvalidInput, "email is too long");
    }
    if (containsControlCharacter(candidate)) {
        return VoidResult::failure(ErrorCode::InvalidInput, "email contains invalid characters");
    }

    // Deliberately permissive: the only reliable proof that an address exists is sending
    // mail to it, which the verification flow already does. Over-strict patterns reject
    // valid addresses, which is the worse failure.
    const auto at = candidate.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= candidate.size()) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "email must contain a local part and a domain");
    }
    if (candidate.find('@', at + 1) != std::string::npos) {
        return VoidResult::failure(ErrorCode::InvalidInput, "email must contain exactly one '@'");
    }
    if (candidate.find_first_of(" \t") != std::string::npos) {
        return VoidResult::failure(ErrorCode::InvalidInput, "email must not contain spaces");
    }

    const auto domain = candidate.substr(at + 1);
    const auto dot = domain.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= domain.size()) {
        return VoidResult::failure(ErrorCode::InvalidInput, "email domain is not valid");
    }
    if (domain.front() == '-' || domain.back() == '-' || domain.back() == '.') {
        return VoidResult::failure(ErrorCode::InvalidInput, "email domain is not valid");
    }

    return VoidResult::success();
}

VoidResult validatePassword(std::string_view password) {
    if (password.size() < MIN_PASSWORD_LENGTH) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "password must be at least " +
                                       std::to_string(MIN_PASSWORD_LENGTH) + " characters");
    }
    if (password.size() > MAX_PASSWORD_LENGTH) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "password must be at most " +
                                       std::to_string(MAX_PASSWORD_LENGTH) + " characters");
    }
    // Whitespace inside a passphrase is fine; a password made only of it is not.
    if (trim(password).empty()) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "password must not consist only of whitespace");
    }
    return VoidResult::success();
}

VoidResult validateDisplayName(std::string_view displayName) {
    const std::string candidate = trim(displayName);

    if (candidate.size() < MIN_DISPLAY_NAME_LENGTH) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "display name must be at least " +
                                       std::to_string(MIN_DISPLAY_NAME_LENGTH) + " characters");
    }
    if (candidate.size() > MAX_DISPLAY_NAME_LENGTH) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "display name must be at most " +
                                       std::to_string(MAX_DISPLAY_NAME_LENGTH) + " characters");
    }
    if (containsControlCharacter(candidate)) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "display name contains invalid characters");
    }
    return VoidResult::success();
}

} // namespace launcher::domain
