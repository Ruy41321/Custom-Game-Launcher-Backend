#include "storage/BlobStore.h"

#include <fstream>
#include <system_error>
#include <utility>

#include "common/Hash.h"
#include "domain/Manifest.h"

namespace launcher::storage {
namespace {

using common::ErrorCode;
using common::VoidResult;

constexpr const char* STAGING_DIRECTORY = "staging";

VoidResult ensureDirectory(const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec && !std::filesystem::is_directory(directory)) {
        return VoidResult::failure(ErrorCode::DependencyFailure,
                                   "cannot create the storage directory: " + ec.message());
    }
    return VoidResult::success();
}

} // namespace

BlobStore::BlobStore(std::filesystem::path root)
    : root_(std::move(root)) {}

std::string BlobStore::storageKeyFor(std::string_view sha256) {
    // Callers validate the hash before reaching storage; refusing to build a path from
    // anything else keeps a malformed value from escaping the root through "../".
    if (!domain::isSha256Hex(sha256)) {
        return {};
    }
    return std::string(sha256.substr(0, 2)) + "/" + std::string(sha256.substr(2, 2)) + "/" +
           std::string(sha256);
}

std::filesystem::path BlobStore::pathFor(std::string_view sha256) const {
    const auto key = storageKeyFor(sha256);
    if (key.empty()) {
        return {};
    }
    return root_ / key;
}

bool BlobStore::contains(std::string_view sha256) const {
    const auto path = pathFor(sha256);
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

std::filesystem::path BlobStore::stagingPathFor(std::string_view sessionId) const {
    // The session id is a server-generated uuid, never client input, so it cannot traverse.
    return root_ / STAGING_DIRECTORY / (std::string(sessionId) + ".part");
}

VoidResult BlobStore::writeAt(const std::filesystem::path& staging,
                              int64_t offset,
                              std::string_view data) const {
    if (offset < 0) {
        return VoidResult::failure(ErrorCode::InvalidInput, "offset must not be negative");
    }
    if (auto prepared = ensureDirectory(staging.parent_path()); !prepared.ok()) {
        return prepared;
    }

    std::error_code ec;
    const bool exists = std::filesystem::is_regular_file(staging, ec);
    if (!exists) {
        if (offset != 0) {
            return VoidResult::failure(ErrorCode::Conflict,
                                       "this upload has no data yet; resume from offset 0");
        }
        std::ofstream create(staging, std::ios::binary | std::ios::trunc);
        if (!create) {
            return VoidResult::failure(ErrorCode::DependencyFailure,
                                       "cannot create the staging file");
        }
    }

    std::fstream file(staging, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        return VoidResult::failure(ErrorCode::DependencyFailure, "cannot open the staging file");
    }

    file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) {
        return VoidResult::failure(ErrorCode::DependencyFailure,
                                   "cannot seek to the resume offset");
    }
    if (!data.empty()) {
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    file.flush();
    if (!file) {
        return VoidResult::failure(ErrorCode::DependencyFailure, "cannot write the uploaded bytes");
    }

    return VoidResult::success();
}

int64_t BlobStore::sizeOf(const std::filesystem::path& path) const {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return 0;
    }
    return static_cast<int64_t>(size);
}

VoidResult BlobStore::commit(const std::filesystem::path& staging, std::string_view sha256) const {
    const auto target = pathFor(sha256);
    if (target.empty()) {
        return VoidResult::failure(ErrorCode::InvalidInput, "not a valid content address");
    }

    auto digest = common::sha256File(staging);
    if (!digest.ok()) {
        discard(staging);
        return VoidResult::failure(digest.error());
    }

    const std::string actual = std::move(digest).value();
    if (std::string_view(actual) != sha256) {
        // The single most important check in the upload path: the client claimed a content
        // address, and the bytes it sent do not add up to it. Nothing corrupted or tampered
        // with reaches the store.
        discard(staging);
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "the uploaded bytes do not hash to the declared sha256");
    }

    if (auto prepared = ensureDirectory(target.parent_path()); !prepared.ok()) {
        discard(staging);
        return prepared;
    }

    std::error_code ec;
    if (std::filesystem::is_regular_file(target, ec)) {
        discard(staging);
        return VoidResult::success();
    }

    std::filesystem::rename(staging, target, ec);
    if (ec) {
        // Losing the race against a concurrent upload of the same content is not a failure:
        // the blob is there, and it is by definition the same bytes.
        if (std::filesystem::is_regular_file(target)) {
            discard(staging);
            return VoidResult::success();
        }
        discard(staging);
        return VoidResult::failure(ErrorCode::DependencyFailure,
                                   "cannot move the blob into storage: " + ec.message());
    }

    return VoidResult::success();
}

void BlobStore::discard(const std::filesystem::path& staging) const {
    std::error_code ec;
    std::filesystem::remove(staging, ec);
}

} // namespace launcher::storage
