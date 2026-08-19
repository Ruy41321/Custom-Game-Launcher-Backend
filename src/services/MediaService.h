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
#include "services/MediaReclaimer.h"
#include "storage/MediaStore.h"

namespace launcher::services {

struct UploadMediaCommand {
    domain::MediaKind kind{domain::MediaKind::Screenshot};
    std::string altText;
    int sortOrder{0};
    /// The file itself — a picture, or a video, which is the same request an order of magnitude
    /// larger. Bounded by whichever of the two MediaLimits applies to the kind, and by Drogon's
    /// own body limit before the controller ever calls in.
    std::string bytes;
};

struct MediaLimits {
    int64_t maxBytes{5LL * 1024 * 1024};
    /// A trailer, not a film, and its own number rather than a multiple of the one above: the
    /// two are refused by the same code path but they are not the same order of magnitude, and a
    /// deployment that wants larger pictures has no reason to be made to accept larger videos.
    int64_t maxVideoBytes{64LL * 1024 * 1024};
};

/// Game artwork: covers, banners, logos, screenshots — and videos, which travel the same route
/// because they are the same problem: one public, content-addressed file that describes a game.
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

    const repositories::IGameRepository& games_;
    const repositories::IMediaRepository& media_;
    storage::MediaStore store_;
    /// Declared after the store because it is built from it.
    MediaReclaimer reclaimer_;
    MediaLimits limits_;
};

} // namespace launcher::services
