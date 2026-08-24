#pragma once

#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

namespace launcher::common {

struct LoggingOptions {
    std::string level{"info"};
    std::string directory; ///< empty disables the rotating file sink
    bool json{true};
    std::size_t maxFileSizeBytes{10U * 1024U * 1024U};
    std::size_t maxFiles{5};
};

/// Installs the global logger. Safe to call more than once; the previous logger is replaced.
void initLogging(const LoggingOptions& options);

/// Escapes a value for embedding inside a JSON-formatted log line. Any dynamic content —
/// user input, file paths, database errors — must go through this.
std::string escapeJson(std::string_view value);

} // namespace launcher::common
