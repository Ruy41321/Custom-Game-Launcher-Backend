#include "common/Time.h"

#include <array>
#include <ctime>

namespace launcher::common {

std::string formatUnixTimeUtc(int64_t secondsSinceEpoch) {
    const auto value = static_cast<std::time_t>(secondsSinceEpoch);

    std::tm broken{};
#ifdef _WIN32
    if (gmtime_s(&broken, &value) != 0) {
        return {};
    }
#else
    if (gmtime_r(&value, &broken) == nullptr) {
        return {};
    }
#endif

    std::array<char, 32> buffer{};
    const auto written = std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &broken);
    return std::string(buffer.data(), written);
}

} // namespace launcher::common
