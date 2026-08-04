#include "services/PatchNoteService.h"

#include <algorithm>
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
constexpr const char* NO_SUCH_NOTE = "no such patch note";
constexpr const char* NO_SUCH_VERSION = "no such version";
constexpr const char* NOT_YOURS = "this game belongs to another publisher";

} // namespace

PatchNoteService::PatchNoteService(const repositories::IGameRepository& games,
                                   const repositories::IGameVersionRepository& versions,
                                   const repositories::IPatchNoteRepository& notes)
    : games_(games),
      versions_(versions),
      notes_(notes) {}

drogon::Task<Result<repositories::PatchNotePage>>
PatchNoteService::listForGame(Actor actor, std::string idOrSlug, int limit, int offset) const {
    using Listing = Result<repositories::PatchNotePage>;

    if (!actor.can(domain::permissions::GAME_READ)) {
        co_return Listing::failure(ErrorCode::Forbidden, "you cannot browse the catalog");
    }

    auto game = domain::isUuid(idOrSlug) ? co_await games_.findById(idOrSlug)
                                         : co_await games_.findBySlug(idOrSlug);
    if (!game.has_value() || !domain::mayViewGame(*game, actor)) {
        co_return Listing::failure(ErrorCode::NotFound, NO_SUCH_GAME);
    }

    repositories::PatchNoteQuery query;
    query.gameId = game->id;
    // A draft is the publisher's own working copy. Everybody else sees the published devlog,
    // whatever the request asked for.
    query.includeUnpublished = domain::mayEditGame(*game, actor);
    query.limit = std::clamp(limit, 1, repositories::MAX_PATCH_NOTE_PAGE_SIZE);
    query.offset = std::max(offset, 0);

    co_return Listing::success(co_await notes_.search(std::move(query)));
}

drogon::Task<Result<domain::PatchNote>>
PatchNoteService::create(Actor actor, std::string gameId, CreatePatchNoteCommand command) const {
    using Created = Result<domain::PatchNote>;

    auto game = domain::isUuid(gameId) ? co_await games_.findById(gameId)
                                       : co_await games_.findBySlug(gameId);
    if (!game.has_value() || !domain::mayViewGame(*game, actor)) {
        co_return Created::failure(ErrorCode::NotFound, NO_SUCH_GAME);
    }
    if (!domain::mayEditGame(*game, actor)) {
        co_return Created::failure(ErrorCode::Forbidden, NOT_YOURS);
    }

    domain::NewPatchNote note;
    note.gameId = game->id;
    note.title = domain::trim(command.title);
    note.bodyMarkdown = command.bodyMarkdown;
    note.authorUserId = actor.userId;
    note.publish = command.publish;
    note.gameVersionId = domain::trim(command.versionId);

    if (auto check = domain::validatePatchNoteTitle(note.title); !check.ok()) {
        co_return Created::failure(check.error());
    }
    if (auto check = domain::validatePatchNoteBody(note.bodyMarkdown); !check.ok()) {
        co_return Created::failure(check.error());
    }

    if (!note.gameVersionId.empty()) {
        // Checked here rather than left to the foreign key: a version of another game would
        // otherwise be a constraint violation, and a violation surfaces as a 500.
        if (!domain::isUuid(note.gameVersionId)) {
            co_return Created::failure(ErrorCode::NotFound, NO_SUCH_VERSION);
        }
        const auto version = co_await versions_.findById(note.gameVersionId);
        if (!version.has_value() || version->gameId != game->id) {
            co_return Created::failure(ErrorCode::NotFound, NO_SUCH_VERSION);
        }
    }

    co_return co_await notes_.create(std::move(note));
}

drogon::Task<Result<domain::PatchNote>>
PatchNoteService::update(Actor actor, std::string noteId, domain::PatchNoteUpdate changes) const {
    using Updated = Result<domain::PatchNote>;

    auto existing = co_await editableNote(actor, noteId);
    if (!existing.ok()) {
        co_return existing;
    }
    if (changes.empty()) {
        co_return existing;
    }

    if (changes.title.has_value()) {
        changes.title = domain::trim(*changes.title);
        if (auto check = domain::validatePatchNoteTitle(*changes.title); !check.ok()) {
            co_return Updated::failure(check.error());
        }
    }
    if (changes.bodyMarkdown.has_value()) {
        if (auto check = domain::validatePatchNoteBody(*changes.bodyMarkdown); !check.ok()) {
            co_return Updated::failure(check.error());
        }
    }
    if (changes.gameVersionId.has_value()) {
        changes.gameVersionId = domain::trim(*changes.gameVersionId);
        if (!changes.gameVersionId->empty()) {
            if (!domain::isUuid(*changes.gameVersionId)) {
                co_return Updated::failure(ErrorCode::NotFound, NO_SUCH_VERSION);
            }
            const auto version = co_await versions_.findById(*changes.gameVersionId);
            if (!version.has_value() || version->gameId != existing.value().gameId) {
                co_return Updated::failure(ErrorCode::NotFound, NO_SUCH_VERSION);
            }
        }
    }

    auto updated = co_await notes_.update(noteId, std::move(changes));
    if (!updated.has_value()) {
        co_return Updated::failure(ErrorCode::NotFound, NO_SUCH_NOTE);
    }
    co_return Updated::success(std::move(*updated));
}

drogon::Task<VoidResult> PatchNoteService::remove(Actor actor, std::string noteId) const {
    auto existing = co_await editableNote(actor, noteId);
    if (!existing.ok()) {
        co_return VoidResult::failure(existing.error());
    }

    if (!co_await notes_.remove(noteId)) {
        co_return VoidResult::failure(ErrorCode::NotFound, NO_SUCH_NOTE);
    }
    co_return VoidResult::success();
}

drogon::Task<Result<domain::PatchNote>> PatchNoteService::editableNote(Actor actor,
                                                                       std::string noteId) const {
    using Found = Result<domain::PatchNote>;

    if (!domain::isUuid(noteId)) {
        co_return Found::failure(ErrorCode::NotFound, NO_SUCH_NOTE);
    }

    auto note = co_await notes_.findById(noteId);
    if (!note.has_value()) {
        co_return Found::failure(ErrorCode::NotFound, NO_SUCH_NOTE);
    }

    auto game = co_await games_.findById(note->gameId);
    if (!game.has_value() || !domain::mayViewGame(*game, actor)) {
        co_return Found::failure(ErrorCode::NotFound, NO_SUCH_NOTE);
    }
    if (!domain::mayEditGame(*game, actor)) {
        co_return Found::failure(ErrorCode::Forbidden, NOT_YOURS);
    }
    co_return Found::success(std::move(*note));
}

} // namespace launcher::services
