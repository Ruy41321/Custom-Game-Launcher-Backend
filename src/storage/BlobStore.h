#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "common/Result.h"

namespace launcher::storage {

/// Content-addressed blob storage on the local filesystem.
///
/// A blob lives at `<root>/ab/cd/<sha256>` — two levels of fan-out taken from the first four
/// hex characters, so no directory ever holds every file in the deployment. Uploads are
/// assembled under `<root>/staging/<sessionId>.part` and only move into place once their
/// bytes hash to the name they claim, which is what makes a resumed upload safe: a partially
/// written file is never reachable by its content address.
///
/// Staging deliberately lives *inside* the root so that the final move is a rename within one
/// filesystem, which is atomic. A staging directory elsewhere would silently degrade to a
/// copy, and a crash mid-copy would publish a truncated blob.
class BlobStore {
  public:
    explicit BlobStore(std::filesystem::path root);

    /// Path relative to the root, as recorded in `blobs.storage_key`. Static because the
    /// nginx file server derives the same path from the same rule without this class.
    static std::string storageKeyFor(std::string_view sha256);

    const std::filesystem::path& root() const { return root_; }

    std::filesystem::path pathFor(std::string_view sha256) const;

    bool contains(std::string_view sha256) const;

    std::filesystem::path stagingPathFor(std::string_view sessionId) const;

    /// Writes `data` at `offset`, creating the staging file when absent.
    ///
    /// Positioned rather than appending: the database hands out the offset a resumed upload
    /// continues at, and writing anywhere else would let the file and that offset disagree.
    common::VoidResult
    writeAt(const std::filesystem::path& staging, int64_t offset, std::string_view data) const;

    /// Size of an existing file, or 0 when it does not exist.
    int64_t sizeOf(const std::filesystem::path& path) const;

    /// Hash-checks the staging file and moves it to its content address.
    ///
    /// A mismatch discards the file and fails — this is the check that keeps a corrupted or
    /// tampered upload out of the store. A blob that is already present is success without a
    /// move: identical content addresses are identical content.
    common::VoidResult commit(const std::filesystem::path& staging, std::string_view sha256) const;

    void discard(const std::filesystem::path& staging) const;

  private:
    std::filesystem::path root_;
};

} // namespace launcher::storage
