#pragma once

#include <drogon/orm/DbClient.h>

#include "repositories/ICrashReportRepository.h"

namespace launcher::repositories::postgres {

/// PostgreSQL implementation for crash reports. See PgSupport.h for the conventions every
/// repository here follows.
class PgCrashReportRepository : public ICrashReportRepository {
  public:
    explicit PgCrashReportRepository(drogon::orm::DbClientPtr database);

    drogon::Task<domain::CrashReport> add(domain::NewCrashReport report,
                                          std::string fingerprint) const override;

    drogon::Task<CrashPage> search(CrashQuery query) const override;

    drogon::Task<CrashGroupPage> groups(int limit, int offset) const override;

    drogon::Task<std::optional<domain::CrashReport>> findById(std::string id) const override;

    drogon::Task<std::size_t> deleteOlderThan(uint32_t seconds) const override;

  private:
    drogon::orm::DbClientPtr database_;
};

} // namespace launcher::repositories::postgres
