#include "services/MediaService.h"

#include <utility>

#include "domain/Catalog.h"
#include "domain/Role.h"
#include "domain/Validation.h"

namespace launcher::services {
namespace {

using common::ErrorCode;
using common::Result;
using common::VoidResult;
using domain::Actor;

constexpr const char* NO_SUCH_GAME = "no such game";
constexpr const char* NO_SUCH_MEDIA = "no such image";
constexpr const char* NOT_YOURS = "this game belongs to another publisher";

} // namespace

MediaService::MediaService(const repositories::IGameRepository& games,
                           const repositories::IMediaRepository& media,
                           storage::MediaStore store,
                           MediaLimits limits)
    : games_(games),
      media_(media),
      store_(std::move(store)),
      reclaimer_(media, store_),
      limits_(limits) {}

drogon::Task<Result<std::vector<domain::GameMedia>>>
MediaService::listForGame(Actor actor, std::string idOrSlug) const {
    using Listing = Result<std::vector<domain::GameMedia>>;

    if (!actor.can(domain::permissions::GAME_READ)) {
        co_return Listing::failure(ErrorCode::Forbidden, "you cannot browse the catalog");
    }

    auto game = domain::isUuid(idOrSlug) ? co_await games_.findById(idOrSlug)
                                         : co_await games_.findBySlug(idOrSlug);
    if (!game.has_value() || !domain::mayViewGame(*game, actor)) {
        co_return Listing::failure(ErrorCode::NotFound, NO_SUCH_GAME);
    }

    co_return Listing::success(co_await media_.listForGame(game->id));
}

drogon::Task<Result<domain::GameMedia>>
MediaService::upload(Actor actor, std::string gameId, UploadMediaCommand command) const {
    using Uploaded = Result<domain::GameMedia>;

    auto game = domain::isUuid(gameId) ? co_await games_.findById(gameId)
                                       : co_await games_.findBySlug(gameId);
    if (!game.has_value() || !domain::mayViewGame(*game, actor)) {
        co_return Uploaded::failure(ErrorCode::NotFound, NO_SUCH_GAME);
    }
    if (!domain::mayEditGame(*game, actor)) {
        co_return Uploaded::failure(ErrorCode::Forbidden, NOT_YOURS);
    }

    if (command.bytes.empty()) {
        co_return Uploaded::failure(ErrorCode::InvalidInput, "the request body is empty");
    }
    if (static_cast<int64_t>(command.bytes.size()) > limits_.maxBytes) {
        co_return Uploaded::failure(ErrorCode::InvalidInput,
                                    "an image must be at most " + std::to_string(limits_.maxBytes) +
                                        " bytes");
    }
    if (auto check = domain::validateAltText(command.altText); !check.ok()) {
        co_return Uploaded::failure(check.error());
    }

    // What the bytes are, not what the request said they are. This value becomes the
    // Content-Type of a public URL, so it is decided here and nowhere else.
    const auto format = domain::sniffImageFormat(command.bytes);
    if (!format.has_value()) {
        co_return Uploaded::failure(ErrorCode::InvalidInput,
                                    "the body is not a PNG, JPEG or WebP image");
    }

    // Counted before anything is written: a refusal that arrives after the file is on disk
    // leaves a blob nothing references.
    if (!domain::isSingletonKind(command.kind)) {
        const auto existing = co_await media_.countForGame(game->id, command.kind);
        if (existing >= domain::MAX_SCREENSHOTS_PER_GAME) {
            co_return Uploaded::failure(ErrorCode::Conflict,
                                        "a game may have at most " +
                                            std::to_string(domain::MAX_SCREENSHOTS_PER_GAME) +
                                            " screenshots");
        }
    }

    auto stored = store_.store(command.bytes, *format);
    if (!stored.ok()) {
        co_return Uploaded::failure(stored.error());
    }

    domain::NewGameMedia row;
    row.gameId = game->id;
    row.kind = command.kind;
    row.storageKey = std::move(stored).value();
    row.sha256 = row.storageKey.substr(6, 64);
    row.contentType = domain::contentTypeOf(*format);
    row.sizeBytes = static_cast<int64_t>(command.bytes.size());
    row.altText = command.altText;
    row.sortOrder = command.sortOrder;

    auto created = co_await media_.create(std::move(row));
    if (!created.ok()) {
        co_return Uploaded::failure(created.error());
    }

    const auto& result = created.value();
    if (!result.replacedStorageKey.empty()) {
        co_await reclaimer_.reclaim(result.replacedStorageKey);
    }
    co_return Uploaded::success(result.media);
}

drogon::Task<Result<domain::GameMedia>>
MediaService::update(Actor actor, std::string mediaId, domain::GameMediaUpdate changes) const {
    using Updated = Result<domain::GameMedia>;

    auto existing = co_await editableMedia(actor, mediaId);
    if (!existing.ok()) {
        co_return existing;
    }
    if (changes.empty()) {
        co_return existing;
    }
    if (changes.altText.has_value()) {
        if (auto check = domain::validateAltText(*changes.altText); !check.ok()) {
            co_return Updated::failure(check.error());
        }
    }

    auto updated = co_await media_.update(mediaId, std::move(changes));
    if (!updated.has_value()) {
        co_return Updated::failure(ErrorCode::NotFound, NO_SUCH_MEDIA);
    }
    co_return Updated::success(std::move(*updated));
}

drogon::Task<VoidResult> MediaService::remove(Actor actor, std::string mediaId) const {
    auto existing = co_await editableMedia(actor, mediaId);
    if (!existing.ok()) {
        co_return VoidResult::failure(existing.error());
    }

    auto removed = co_await media_.remove(mediaId);
    if (!removed.has_value()) {
        co_return VoidResult::failure(ErrorCode::NotFound, NO_SUCH_MEDIA);
    }

    co_await reclaimer_.reclaim(removed->storageKey);
    co_return VoidResult::success();
}

drogon::Task<Result<domain::GameMedia>> MediaService::editableMedia(Actor actor,
                                                                    std::string mediaId) const {
    using Found = Result<domain::GameMedia>;

    if (!domain::isUuid(mediaId)) {
        co_return Found::failure(ErrorCode::NotFound, NO_SUCH_MEDIA);
    }

    auto media = co_await media_.findById(mediaId);
    if (!media.has_value()) {
        co_return Found::failure(ErrorCode::NotFound, NO_SUCH_MEDIA);
    }

    auto game = co_await games_.findById(media->gameId);
    if (!game.has_value() || !domain::mayViewGame(*game, actor)) {
        co_return Found::failure(ErrorCode::NotFound, NO_SUCH_MEDIA);
    }
    if (!domain::mayEditGame(*game, actor)) {
        co_return Found::failure(ErrorCode::Forbidden, NOT_YOURS);
    }
    co_return Found::success(std::move(*media));
}

} // namespace launcher::services
