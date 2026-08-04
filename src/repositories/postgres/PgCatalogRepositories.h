#pragma once

#include <drogon/orm/DbClient.h>

#include "repositories/IBuildRepository.h"
#include "repositories/IGameRepository.h"
#include "repositories/IGameVersionRepository.h"
#include "repositories/ILibraryRepository.h"
#include "repositories/IMediaRepository.h"

namespace launcher::repositories::postgres {

/// PostgreSQL implementations for the catalog: games, versions, builds and libraries.
/// See PgSupport.h for the conventions every repository here follows.

class PgGameRepository : public IGameRepository {
  public:
    explicit PgGameRepository(drogon::orm::DbClientPtr database);

    drogon::Task<std::optional<domain::Game>> findById(std::string id) const override;

    drogon::Task<std::optional<domain::Game>> findBySlug(std::string slug) const override;

    drogon::Task<common::Result<domain::Game>> create(domain::NewGame game) const override;

    drogon::Task<std::optional<domain::Game>> update(std::string id,
                                                     domain::GameUpdate changes) const override;

    drogon::Task<GamePage> search(GameQuery query) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

class PgGameVersionRepository : public IGameVersionRepository {
  public:
    explicit PgGameVersionRepository(drogon::orm::DbClientPtr database);

    drogon::Task<common::Result<domain::GameVersion>>
    create(domain::NewGameVersion version) const override;

    drogon::Task<std::optional<domain::GameVersion>> findById(std::string id) const override;

    drogon::Task<std::vector<domain::GameVersion>>
    listForGame(std::string gameId, bool includeUnpublished) const override;

    drogon::Task<bool> publish(std::string id) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

class PgBuildRepository : public IBuildRepository {
  public:
    explicit PgBuildRepository(drogon::orm::DbClientPtr database);

    drogon::Task<common::Result<domain::Build>> create(domain::NewBuild build) const override;

    drogon::Task<std::optional<domain::Build>> findById(std::string id) const override;

    drogon::Task<std::vector<domain::Build>>
    listForVersions(std::vector<std::string> versionIds) const override;

    drogon::Task<std::optional<domain::BuildOwnership>>
    findOwnership(std::string buildId) const override;

    drogon::Task<std::vector<domain::ManifestEntry>> filesFor(std::string buildId) const override;

    drogon::Task<std::optional<domain::Build>> finalize(std::string buildId,
                                                        FinalizedManifest manifest) const override;

    drogon::Task<bool> markFailed(std::string buildId) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

class PgMediaRepository : public IMediaRepository {
  public:
    explicit PgMediaRepository(drogon::orm::DbClientPtr database);

    drogon::Task<common::Result<Stored>> create(domain::NewGameMedia media) const override;

    drogon::Task<std::vector<domain::GameMedia>> listForGame(std::string gameId) const override;

    drogon::Task<std::optional<domain::GameMedia>> findById(std::string id) const override;

    drogon::Task<std::optional<domain::GameMedia>>
    update(std::string id, domain::GameMediaUpdate changes) const override;

    drogon::Task<std::optional<domain::GameMedia>> remove(std::string id) const override;

    drogon::Task<int> countForGame(std::string gameId, domain::MediaKind kind) const override;

    drogon::Task<bool> isStorageKeyReferenced(std::string storageKey) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

class PgLibraryRepository : public ILibraryRepository {
  public:
    explicit PgLibraryRepository(drogon::orm::DbClientPtr database);

    drogon::Task<bool> add(std::string userId, std::string gameId) const override;

    drogon::Task<bool> remove(std::string userId, std::string gameId) const override;

    drogon::Task<bool> contains(std::string userId, std::string gameId) const override;

    drogon::Task<std::vector<domain::Game>> list(std::string userId) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

} // namespace launcher::repositories::postgres
