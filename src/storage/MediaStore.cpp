#include "storage/MediaStore.h"

#include <fstream>
#include <system_error>
#include <utility>

#include "common/Hash.h"
#include "domain/Manifest.h"

namespace launcher::storage {
namespace {

using common::ErrorCode;
using common::Result;
using common::VoidResult;

constexpr const char* STAGING_DIRECTORY = "staging";

VoidResult ensureDirectory(const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec && !std::filesystem::is_directory(directory)) {
        return VoidResult::failure(ErrorCode::DependencyFailure,
                                   "cannot create the media directory: " + ec.message());
    }
    return VoidResult::success();
}

} // namespace

MediaStore::MediaStore(std::filesystem::path root)
    : root_(std::move(root)) {}

std::string MediaStore::storageKeyFor(std::string_view sha256, domain::StoredFormat format) {
    if (!domain::isSha256Hex(sha256)) {
        return {};
    }
    return std::string(sha256.substr(0, 2)) + "/" + std::string(sha256.substr(2, 2)) + "/" +
           std::string(sha256) + "." + format.extension;
}

std::filesystem::path MediaStore::pathFor(std::string_view storageKey) const {
    // Storage keys are derived from a hash by this class and are never client input, but the
    // sweep reads them back out of the database, so a key that is not the shape this produced
    // resolves to nothing rather than to a path outside the root.
    if (storageKey.empty() || storageKey.find("..") != std::string_view::npos ||
        storageKey.front() == '/') {
        return {};
    }
    return root_ / storageKey;
}

bool MediaStore::contains(std::string_view storageKey) const {
    const auto path = pathFor(storageKey);
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

Result<std::string> MediaStore::store(std::string_view bytes, domain::StoredFormat format) const {
    const auto digest = common::sha256Hex(bytes);
    const auto key = storageKeyFor(digest, format);
    if (key.empty()) {
        return Result<std::string>::failure(ErrorCode::Internal, "cannot derive a storage key");
    }

    const auto target = root_ / key;
    std::error_code ec;
    if (std::filesystem::is_regular_file(target, ec)) {
        // Identical content addresses are identical content: two games sharing a cover share
        // the file, and re-uploading the same picture writes nothing.
        return Result<std::string>::success(key);
    }

    if (auto prepared = ensureDirectory(root_ / STAGING_DIRECTORY); !prepared.ok()) {
        return Result<std::string>::failure(prepared.error());
    }
    if (auto prepared = ensureDirectory(target.parent_path()); !prepared.ok()) {
        return Result<std::string>::failure(prepared.error());
    }

    // Staging inside the root keeps the publish step a rename within one filesystem, which is
    // atomic. Writing straight to the target would make a crash mid-write leave a truncated
    // image reachable at its own content address.
    const auto staging = root_ / STAGING_DIRECTORY / (digest + "." + format.extension);
    {
        std::ofstream file(staging, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Result<std::string>::failure(ErrorCode::DependencyFailure,
                                                "cannot create the media staging file");
        }
        if (!bytes.empty()) {
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        file.flush();
        if (!file) {
            std::filesystem::remove(staging, ec);
            return Result<std::string>::failure(ErrorCode::DependencyFailure,
                                                "cannot write the uploaded file");
        }
    }

    std::filesystem::rename(staging, target, ec);
    if (ec) {
        // Losing the race against a concurrent upload of the same picture is not a failure.
        if (std::filesystem::is_regular_file(target)) {
            std::error_code cleanup;
            std::filesystem::remove(staging, cleanup);
            return Result<std::string>::success(key);
        }
        std::error_code cleanup;
        std::filesystem::remove(staging, cleanup);
        return Result<std::string>::failure(ErrorCode::DependencyFailure,
                                            "cannot move the file into storage: " + ec.message());
    }

    return Result<std::string>::success(key);
}

void MediaStore::remove(std::string_view storageKey) const {
    const auto path = pathFor(storageKey);
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace launcher::storage
