#pragma once

#include <drogon/utils/coroutine.h>

#include <string>
#include <vector>

#include "repositories/IMediaRepository.h"
#include "storage/MediaStore.h"

namespace launcher::services {

/// Removes an artwork file once, and only once, nothing points at it any more.
///
/// A storage key is a content address, so two games with the same picture are one file. A row
/// going away therefore says nothing about whether the bytes are still in use, and deleting the
/// file unconditionally would blank the other game's cover. The rule is three lines long, which
/// is exactly why it lives in one place: two services now delete artwork — MediaService when a
/// publisher removes a picture, CatalogService when a whole game goes — and a rule with two
/// implementations stops being a rule the first time one of them is edited.
///
/// Copyable on purpose: it holds a repository reference and a path, so the composition root can
/// hand a copy to each service that needs one.
class MediaReclaimer {
  public:
    MediaReclaimer(const repositories::IMediaRepository& media, storage::MediaStore store);

    drogon::Task<> reclaim(std::string storageKey) const;

    /// Each key is asked about separately: a game's cover and one of its screenshots can be the
    /// same picture, and so can two games'.
    drogon::Task<> reclaimAll(std::vector<std::string> storageKeys) const;

  private:
    const repositories::IMediaRepository& media_;
    storage::MediaStore store_;
};

} // namespace launcher::services
