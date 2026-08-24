#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace launcher::testing {

/// A directory under the system temp path that removes itself at end of scope.
class TemporaryDirectory {
  public:
    explicit TemporaryDirectory(const std::string& prefix = "launcher-test") {
        static std::atomic<unsigned long long> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                (prefix + "-" + std::to_string(counter.fetch_add(1)) + "-" +
                 std::to_string(static_cast<unsigned long long>(
                     std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

    const std::filesystem::path& path() const { return path_; }

    std::filesystem::path writeFile(const std::string& name, const std::string& contents) const {
        const auto file = path_ / name;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        out << contents;
        return file;
    }

  private:
    std::filesystem::path path_;
};

} // namespace launcher::testing
