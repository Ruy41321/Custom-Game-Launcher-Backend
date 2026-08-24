#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "common/Result.h"

namespace launcher::common {

/// Resolves a variable name to its value, or nullopt when it is not set.
using EnvLookup = std::function<std::optional<std::string>(const std::string&)>;

/// Reads the real process environment.
EnvLookup systemEnvLookup();

/// Expands `${VAR}` and `${VAR:-default}` occurrences in `input`.
///
/// A `${VAR}` with no value and no default is an error rather than an empty string: a
/// silently blank database password or JWT secret is exactly the failure mode that must
/// never reach production. Use `${VAR:-}` to opt into an empty default explicitly.
Result<std::string> interpolateEnv(std::string_view input, const EnvLookup& lookup);

} // namespace launcher::common
