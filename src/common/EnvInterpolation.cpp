#include "common/EnvInterpolation.h"

#include <cstdlib>

namespace launcher::common {

EnvLookup systemEnvLookup() {
    return [](const std::string& name) -> std::optional<std::string> {
        const char* value = std::getenv(name.c_str());
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::string(value);
    };
}

Result<std::string> interpolateEnv(std::string_view input, const EnvLookup& lookup) {
    std::string output;
    output.reserve(input.size());

    std::size_t cursor = 0;
    while (cursor < input.size()) {
        const auto start = input.find("${", cursor);
        if (start == std::string_view::npos) {
            output.append(input.substr(cursor));
            break;
        }

        output.append(input.substr(cursor, start - cursor));

        const auto end = input.find('}', start + 2);
        if (end == std::string_view::npos) {
            return Result<std::string>::failure(ErrorCode::InvalidInput,
                                                "unterminated ${...} placeholder in configuration");
        }

        const auto expression = input.substr(start + 2, end - start - 2);
        const auto separator = expression.find(":-");

        const std::string name(
            separator == std::string_view::npos ? expression : expression.substr(0, separator));
        if (name.empty()) {
            return Result<std::string>::failure(ErrorCode::InvalidInput,
                                                "empty variable name in ${...} placeholder");
        }

        if (auto value = lookup(name); value.has_value()) {
            output.append(*value);
        } else if (separator != std::string_view::npos) {
            output.append(expression.substr(separator + 2));
        } else {
            return Result<std::string>::failure(
                ErrorCode::InvalidInput, "required environment variable is not set: " + name);
        }

        cursor = end + 1;
    }

    return Result<std::string>::success(std::move(output));
}

} // namespace launcher::common
