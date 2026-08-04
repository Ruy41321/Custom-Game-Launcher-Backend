#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/Result.h"
#include "domain/Actor.h"
#include "domain/Media.h"
#include "repositories/IGameRepository.h"
#include "repositories/IMediaRepository.h"
#include "storage/MediaStore.h"

namespace launcher::services {

struct UploadMediaCommand {
    domain::MediaKind kind{domain::MediaKind::Screenshot};
    std::string altText;
    int sortOrder{0};
    /// The image itself. Small enough to arrive whole, and bounded by MediaLimits::maxBytes
    /// before the controller ever calls in.
    std::string bytes;
};

struct MediaLimits {
    int64_t maxBytes{5LL * 1024 * 1024};
};

/// Game artwork: covers, banners, logos and screenshots.
///
/// Separate from CatalogService because it owns a different kind of storage and a different
/// failure mode — a write here touches the filesystem — while sharing the authorization rules,
/// which live in `domain::mayViewGame` / `domain::mayEditGame` so that neither service can
/// drift from the other.
class MediaService {
  public:
    MediaService(const repositories::IGameRepository& games,
                 const repositories::IMediaRepository& media,
                 storage::MediaStore store,
                 MediaLimits limits);

    /// Everything hanging off a game the actor may see. Artwork is as public as the game is.
    drogon::Task<common::Result<std::vector<domain::GameMedia>>>
    listForGame(domain::Actor actor, std::string idOrSlug) const;

    drogon::Task<common::Result<domain::GameMedia>>
    upload(domain::Actor actor, std::string gameId, UploadMediaCommand command) const;

    drogon::Task<common::Result<domain::GameMedia>>
    update(domain::Actor actor, std::string mediaId, domain::GameMediaUpdate changes) const;

    drogon::Task<common::VoidResult> remove(domain::Actor actor, std::string mediaId) const;

  private:
    /// Resolves the game behind a media row and checks the caller may change it. A row whose
    /// game the caller cannot see is reported missing, never forbidden.
    drogon::Task<common::Result<domain::GameMedia>> editableMedia(domain::Actor actor,
                                                                  std::string mediaId) const;

    /// Deletes the file behind a storage key, but only once no row points at it.
    drogon::Task<> removeUnreferencedFile(std::string storageKey) const;

    const repositories::IGameRepository& games_;
    const repositories::IMediaRepository& media_;
    storage::MediaStore store_;
    MediaLimits limits_;
};

} // namespace launcher::services
