#pragma once

#include <drogon/utils/coroutine.h>

#include <optional>
#include <string>
#include <vector>

#include "common/Result.h"
#include "domain/Actor.h"
#include "domain/Catalog.h"
#include "repositories/IBuildRepository.h"
#include "repositories/IGameRepository.h"
#include "repositories/IGameVersionRepository.h"
#include "repositories/ILibraryRepository.h"

namespace launcher::services {

struct CreateGameCommand {
    std::string title;
    std::string slug; ///< derived from the title when empty
    std::string summary;
    std::string description;
    std::string releaseDate;
    domain::GameVisibility visibility{domain::GameVisibility::Draft};
};

struct CreateVersionCommand {
    std::string semver;
    domain::BuildStage stage{domain::BuildStage::Release};
    std::string releaseNotes;
    bool publish{false};
};

struct CreateBuildCommand {
    std::string versionId;
    domain::BuildPlatform platform{domain::BuildPlatform::Windows};
    domain::BuildArchitecture architecture{domain::BuildArchitecture::X64};
};

/// The catalog: publishing games and versions, Explore, and the server-side library.
///
/// Every rule here is an authorization rule the client also implements for its own UX. That
/// copy is advisory. This one is the authority, and it is written to hold even when the
/// caller has crafted the request by hand.
class CatalogService {
  public:
    CatalogService(const repositories::IGameRepository& games,
                   const repositories::IGameVersionRepository& versions,
                   const repositories::IBuildRepository& builds,
                   const repositories::ILibraryRepository& library);

    drogon::Task<common::Result<domain::Game>> createGame(domain::Actor actor,
                                                          CreateGameCommand command) const;

    drogon::Task<common::Result<domain::Game>>
    updateGame(domain::Actor actor, std::string gameId, domain::GameUpdate changes) const;

    /// Accepts either a uuid or a slug, so a catalog URL can be human-readable.
    drogon::Task<common::Result<domain::GameDetail>> gameDetail(domain::Actor actor,
                                                                std::string idOrSlug) const;

    /// Explore. Only published games, whoever is asking.
    drogon::Task<common::Result<repositories::GamePage>>
    explore(domain::Actor actor, repositories::GameQuery query) const;

    /// The publisher's own dashboard, drafts included.
    drogon::Task<common::Result<repositories::GamePage>>
    publishedByActor(domain::Actor actor, repositories::GameQuery query) const;

    drogon::Task<common::Result<domain::GameVersion>>
    createVersion(domain::Actor actor, std::string gameId, CreateVersionCommand command) const;

    drogon::Task<common::Result<domain::Build>>
    createBuild(domain::Actor actor, std::string gameId, CreateBuildCommand command) const;

    drogon::Task<common::VoidResult> addToLibrary(domain::Actor actor, std::string gameId) const;

    drogon::Task<common::VoidResult> removeFromLibrary(domain::Actor actor,
                                                       std::string gameId) const;

    drogon::Task<common::Result<std::vector<domain::Game>>> library(domain::Actor actor) const;

    /// Resolves a game the actor is allowed to see, or the reason they are not.
    drogon::Task<common::Result<domain::Game>> visibleGame(domain::Actor actor,
                                                           std::string idOrSlug) const;

  private:
    /// Loads a game the actor is allowed to *modify*. Ownership is checked here rather than in
    /// the controllers so that no route can forget it.
    drogon::Task<common::Result<domain::Game>> editableGame(domain::Actor actor,
                                                            std::string gameId) const;

    const repositories::IGameRepository& games_;
    const repositories::IGameVersionRepository& versions_;
    const repositories::IBuildRepository& builds_;
    const repositories::ILibraryRepository& library_;
};

} // namespace launcher::services
