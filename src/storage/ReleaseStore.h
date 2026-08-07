#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "common/Result.h"

namespace launcher::storage {

/// Content-addressed storage for launcher release artifacts, on its own root.
///
/// The third such class, and the third root, for the reason the second one exists: **what a
/// root is served as decides which root it is.** `/data/blobs` is behind nginx's `secure_link`
/// because a build belongs to whoever published it; `/data/media` is public and unsigned
/// because a cover is public; this one is public and unsigned because a launcher binary is the
/// most public thing in the deployment — it is MIT-licensed software everybody downloads, and
/// the client asking for it has no token to spend on being handed a signed URL, because the
/// launcher that most needs an update is the one that cannot sign in yet.
///
/// Serving it unsigned is not a weakening. The integrity guarantee here comes from the
/// signature over the release document and the content address inside it, neither of which a
/// URL participates in: an attacker who could rewrite the URL entirely would still have to
/// produce bytes hashing to a value somebody signed.
///
/// The difference from MediaStore is the size. An image arrives whole in a request body and is
/// hashed in memory; a self-contained launcher is tens of megabytes and arrives as a file the
/// operator put on disk, so it is hashed by streaming and copied rather than held.
class ReleaseStore {
  public:
    explicit ReleaseStore(std::filesystem::path root);

    /// `ab/cd/<sha256>.zip`, or empty when the hash is not one. The extension is part of the
    /// key so nginx answers from its own mime table, as it does for artwork.
    static std::string storageKeyFor(std::string_view sha256);

    const std::filesystem::path& root() const { return root_; }

    std::filesystem::path pathFor(std::string_view storageKey) const;

    bool contains(std::string_view storageKey) const;

    /// Hashes `source` and stores it under the content address its bytes actually have.
    ///
    /// `expectedSha256` is the hash the signed document names, and a mismatch is a failure
    /// rather than a rename: it means the file on disk is not the file that was signed. This is
    /// the same rule `BlobStore::commit` applies to an upload, arriving from the other side —
    /// nothing reaches its content address until its bytes hash to it.
    common::Result<std::string> store(const std::filesystem::path& source,
                                      std::string_view expectedSha256) const;

    void remove(std::string_view storageKey) const;

  private:
    std::filesystem::path root_;
};

} // namespace launcher::storage
