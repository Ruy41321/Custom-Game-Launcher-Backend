#include "storage/ReleaseStore.h"

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
constexpr const char* ARTIFACT_EXTENSION = ".zip";

VoidResult ensureDirectory(const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec && !std::filesystem::is_directory(directory)) {
        return VoidResult::failure(ErrorCode::DependencyFailure,
                                   "cannot create the release directory: " + ec.message());
    }
    return VoidResult::success();
}

} // namespace

ReleaseStore::ReleaseStore(std::filesystem::path root)
    : root_(std::move(root)) {}

std::string ReleaseStore::storageKeyFor(std::string_view sha256) {
    if (!domain::isSha256Hex(sha256)) {
        return {};
    }
    return std::string(sha256.substr(0, 2)) + "/" + std::string(sha256.substr(2, 2)) + "/" +
           std::string(sha256) + ARTIFACT_EXTENSION;
}

std::filesystem::path ReleaseStore::pathFor(std::string_view storageKey) const {
    if (storageKey.empty() || storageKey.find("..") != std::string_view::npos ||
        storageKey.front() == '/') {
        return {};
    }
    return root_ / storageKey;
}

bool ReleaseStore::contains(std::string_view storageKey) const {
    const auto path = pathFor(storageKey);
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

Result<std::string> ReleaseStore::store(const std::filesystem::path& source,
                                        std::string_view expectedSha256) const {
    const auto key = storageKeyFor(expectedSha256);
    if (key.empty()) {
        return Result<std::string>::failure(ErrorCode::InvalidInput,
                                            "the expected hash is not a SHA-256 digest");
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) {
        return Result<std::string>::failure(ErrorCode::InvalidInput,
                                            "no such file: " + source.string());
    }

    // Streamed, because the file is a whole self-contained runtime and holding it would mean
    // holding it twice — once to hash and once to copy.
    auto digest = common::sha256File(source);
    if (!digest.ok()) {
        return Result<std::string>::failure(digest.error());
    }
    const auto actual = std::move(digest).value();
    if (actual != expectedSha256) {
        return Result<std::string>::failure(
            ErrorCode::InvalidInput,
            "the artifact does not match the hash in the signed document: it hashes to " + actual +
                ", the document names " + std::string(expectedSha256));
    }

    const auto target = root_ / key;
    if (std::filesystem::is_regular_file(target, ec)) {
        // The same bytes are the same file. Publishing a release whose artifact is byte for
        // byte one already stored writes nothing, which is what makes re-running a failed
        // publish free rather than a second copy.
        return Result<std::string>::success(key);
    }

    if (auto prepared = ensureDirectory(root_ / STAGING_DIRECTORY); !prepared.ok()) {
        return Result<std::string>::failure(prepared.error());
    }
    if (auto prepared = ensureDirectory(target.parent_path()); !prepared.ok()) {
        return Result<std::string>::failure(prepared.error());
    }

    // Copied into staging *inside* the root and then renamed, so the move into place is atomic
    // within one filesystem. The source is wherever the operator put it — very possibly another
    // mount — so copying straight to the target would leave a truncated artifact reachable at a
    // content address if the process died halfway.
    const auto staging = root_ / STAGING_DIRECTORY / (actual + ARTIFACT_EXTENSION);
    std::filesystem::remove(staging, ec);
    std::filesystem::copy_file(
        source, staging, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return Result<std::string>::failure(ErrorCode::DependencyFailure,
                                            "cannot stage the artifact: " + ec.message());
    }

    std::filesystem::rename(staging, target, ec);
    if (ec) {
        std::error_code cleanup;
        if (std::filesystem::is_regular_file(target, cleanup)) {
            std::filesystem::remove(staging, cleanup);
            return Result<std::string>::success(key);
        }
        std::filesystem::remove(staging, cleanup);
        return Result<std::string>::failure(
            ErrorCode::DependencyFailure, "cannot move the artifact into storage: " + ec.message());
    }

    return Result<std::string>::success(key);
}

void ReleaseStore::remove(std::string_view storageKey) const {
    const auto path = pathFor(storageKey);
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace launcher::storage
