#include "common/Logging.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>

namespace launcher::common {
namespace {

spdlog::level::level_enum parseLevel(std::string_view level) {
    std::string normalised(level);
    std::transform(normalised.begin(), normalised.end(), normalised.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (normalised == "trace") {
        return spdlog::level::trace;
    }
    if (normalised == "debug") {
        return spdlog::level::debug;
    }
    if (normalised == "warn" || normalised == "warning") {
        return spdlog::level::warn;
    }
    if (normalised == "error") {
        return spdlog::level::err;
    }
    if (normalised == "off") {
        return spdlog::level::off;
    }
    return spdlog::level::info;
}

// %v carries the already-escaped message, so the line stays valid JSON.
constexpr const char* JSON_PATTERN =
    R"({"ts":"%Y-%m-%dT%H:%M:%S.%e%z","level":"%l","thread":%t,"message":"%v"})";
constexpr const char* TEXT_PATTERN = "%^[%Y-%m-%d %H:%M:%S.%e] [%l] [%t]%$ %v";

} // namespace

void initLogging(const LoggingOptions& options) {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    if (!options.directory.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(options.directory, ec);
        if (!ec) {
            const auto file = std::filesystem::path(options.directory) / "launcher-api.log";
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                file.string(), options.maxFileSizeBytes, options.maxFiles));
        }
    }

    auto logger = std::make_shared<spdlog::logger>("launcher", sinks.begin(), sinks.end());
    logger->set_pattern(options.json ? JSON_PATTERN : TEXT_PATTERN);
    logger->set_level(parseLevel(options.level));
    logger->flush_on(spdlog::level::warn);

    spdlog::set_default_logger(logger);
}

std::string escapeJson(std::string_view value) {
    std::ostringstream out;
    for (const char rawChar : value) {
        const auto byte = static_cast<unsigned char>(rawChar);
        switch (rawChar) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (byte < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(byte) << std::dec;
            } else {
                out << rawChar;
            }
            break;
        }
    }
    return out.str();
}

} // namespace launcher::common
