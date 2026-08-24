#pragma once

#include <drogon/utils/coroutine.h>

#include <string>

#include "common/Result.h"
#include "domain/Actor.h"
#include "domain/PatchNote.h"
#include "repositories/IGameRepository.h"
#include "repositories/IGameVersionRepository.h"
#include "repositories/IPatchNoteRepository.h"

namespace launcher::services {

struct CreatePatchNoteCommand {
    std::string title;
    std::string bodyMarkdown;
    /// Optional. When set it must name a version of the same game, which is checked here
    /// rather than left to the foreign key: a version of somebody else's game is a 404, and a
    /// constraint violation would be a 500.
    std::string versionId;
    bool publish{false};
};

/// A game's devlog.
///
/// Separate from CatalogService because nothing else needs it and it would only make that
/// class larger, and safe to separate because the authorization rules live in
/// `domain::mayViewGame` / `domain::mayEditGame` rather than in either service.
class PatchNoteService {
  public:
    PatchNoteService(const repositories::IGameRepository& games,
                     const repositories::IGameVersionRepository& versions,
                     const repositories::IPatchNoteRepository& notes);

    /// Published notes for everybody; the publisher and an operator also see their drafts.
    drogon::Task<common::Result<repositories::PatchNotePage>>
    listForGame(domain::Actor actor, std::string idOrSlug, int limit, int offset) const;

    drogon::Task<common::Result<domain::PatchNote>>
    create(domain::Actor actor, std::string gameId, CreatePatchNoteCommand command) const;

    drogon::Task<common::Result<domain::PatchNote>>
    update(domain::Actor actor, std::string noteId, domain::PatchNoteUpdate changes) const;

    drogon::Task<common::VoidResult> remove(domain::Actor actor, std::string noteId) const;

  private:
    /// Resolves the note and checks the caller may change its game. A note whose game the
    /// caller cannot see is reported missing, never forbidden.
    drogon::Task<common::Result<domain::PatchNote>> editableNote(domain::Actor actor,
                                                                 std::string noteId) const;

    const repositories::IGameRepository& games_;
    const repositories::IGameVersionRepository& versions_;
    const repositories::IPatchNoteRepository& notes_;
};

} // namespace launcher::services
