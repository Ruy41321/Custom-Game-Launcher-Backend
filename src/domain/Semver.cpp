#include "domain/Semver.h"

#include <cctype>
#include <vector>

#include "domain/Validation.h"
#include "domain/ValidationRules.h"

namespace launcher::domain {
namespace {

using common::invalidInput;
using common::Result;

common::VoidResult parseComponent(std::string_view text, int& out) {
    if (text.empty()) {
        return common::VoidResult::failure(
            invalidInput("version components must not be empty", rules::VERSION_INVALID));
    }
    if (text.size() > 1 && text.front() == '0') {
        return common::VoidResult::failure(
            invalidInput("version components must not have leading zeros", rules::VERSION_INVALID));
    }

    int value = 0;
    for (const char character : text) {
        if (std::isdigit(static_cast<unsigned char>(character)) == 0) {
            return common::VoidResult::failure(
                invalidInput("version components must be numeric", rules::VERSION_INVALID));
        }
        value = value * 10 + (character - '0');
        if (value > MAX_VERSION_COMPONENT) {
            return common::VoidResult::failure(
                invalidInput("version components are too large",
                             rules::VERSION_TOO_LARGE,
                             {std::to_string(MAX_VERSION_COMPONENT)}));
        }
    }

    out = value;
    return common::VoidResult::success();
}

} // namespace

Result<Semver> parseSemver(std::string_view input) {
    const auto text = trim(input);
    if (text.empty()) {
        return Result<Semver>::failure(
            invalidInput("a version is required", rules::VERSION_REQUIRED));
    }

    std::vector<std::string_view> parts;
    std::size_t start = 0;
    const std::string_view view{text};
    while (true) {
        const auto separator = view.find('.', start);
        parts.push_back(view.substr(start, separator - start));
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }

    if (parts.size() > 3) {
        return Result<Semver>::failure(invalidInput(
            "a version has at most three components, e.g. 1.2.3", rules::VERSION_INVALID));
    }

    Semver version;
    version.text = text;

    int* const targets[] = {&version.major, &version.minor, &version.patch};
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (auto parsed = parseComponent(parts[index], *targets[index]); !parsed.ok()) {
            return Result<Semver>::failure(parsed.error());
        }
    }

    return Result<Semver>::success(std::move(version));
}

} // namespace launcher::domain
