#pragma once

#include <drogon/orm/DbClient.h>

#include "repositories/ILauncherReleaseRepository.h"

namespace launcher::repositories::postgres {

class PgLauncherReleaseRepository : public ILauncherReleaseRepository {
  public:
    explicit PgLauncherReleaseRepository(drogon::orm::DbClientPtr database);

    drogon::Task<std::optional<domain::LauncherRelease>>
    findLatest(domain::ReleaseQuery query) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

} // namespace launcher::repositories::postgres
