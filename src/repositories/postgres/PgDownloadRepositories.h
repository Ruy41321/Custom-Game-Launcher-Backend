#pragma once

#include <drogon/orm/DbClient.h>

#include "repositories/IDownloadRepository.h"

namespace launcher::repositories::postgres {

/// See PgSupport.h for the conventions every repository here follows.
class PgDownloadRepository : public IDownloadRepository {
  public:
    explicit PgDownloadRepository(drogon::orm::DbClientPtr database);

    drogon::Task<bool> recordEvent(DownloadEvent event) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

} // namespace launcher::repositories::postgres
