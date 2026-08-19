#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "common/Result.h"
#include "domain/Media.h"

namespace launcher::storage {

/// Content-addressed storage for game artwork, on its own root.
///
/// The layout deliberately mirrors BlobStore — two levels of fan-out taken from the hash —
/// but this is a separate class and a separate root rather than a second instance of that
/// one, for two reasons that are both about what the two roots are for.
///
/// The key ends in a file extension, because these files are served by nginx from its own
/// mime table and a cover that arrives as `application/octet-stream` is a cover no client
/// renders. And the root is separate because *this* one is published without a signature: a
/// public location over the build blobs would hand out every game's files to anyone who knew
/// a hash.
///
/// There is no staging protocol here. An image — or a video, which is the same problem an order
/// of magnitude larger — arrives whole in one request, so the write is a temporary file, a hash
/// check, and a rename: the same guarantee the blob store gives across many requests, without
/// the machinery for resuming.
class MediaStore {
  public:
    explicit MediaStore(std::filesystem::path root);

    /// `ab/cd/<sha256>.<ext>`, or empty when the hash is not one.
    ///
    /// Takes a `StoredFormat` rather than one of the two format enums so that there is one
    /// write path for a picture and for a video: what this class needs is an extension and a
    /// content type, and which enum the caller sniffed with is the caller's business.
    static std::string storageKeyFor(std::string_view sha256, domain::StoredFormat format);

    const std::filesystem::path& root() const { return root_; }

    std::filesystem::path pathFor(std::string_view storageKey) const;

    bool contains(std::string_view storageKey) const;

    /// Writes `bytes` and returns the storage key they took.
    ///
    /// The hash is computed here, from the bytes about to be written, rather than accepted
    /// from a caller: an image is small enough that there is nothing to gain from trusting a
    /// declared address, and the file ends up on a public URL.
    common::Result<std::string> store(std::string_view bytes, domain::StoredFormat format) const;

    /// Removes the file behind a storage key. Silent when it is already gone, which is the
    /// normal outcome of a sweep that raced another one.
    void remove(std::string_view storageKey) const;

  private:
    std::filesystem::path root_;
};

} // namespace launcher::storage
