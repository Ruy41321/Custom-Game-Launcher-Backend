#pragma once

#include <drogon/orm/DbClient.h>

#include <memory>

#include "app/Config.h"

namespace launcher::app {

/// Composition root.
///
/// Drogon instantiates controllers itself, so there is no place to inject dependencies into
/// them. This context is built once during startup, owns the configuration and the database
/// client, and will own the service instances as they are added. Controllers read from it;
/// services themselves are always constructed with their collaborators injected, so unit
/// tests bypass this class entirely.
class AppContext {
  public:
    static AppContext& instance();

    void initialize(AppConfig config, drogon::orm::DbClientPtr database);

    bool initialized() const noexcept;

    const AppConfig& config() const;

    const drogon::orm::DbClientPtr& database() const;

    /// Test hook: drops all wiring so a fresh context can be installed.
    void reset();

    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;
    AppContext(AppContext&&) = delete;
    AppContext& operator=(AppContext&&) = delete;

  private:
    AppContext() = default;
    ~AppContext() = default;

    bool initialized_{false};
    AppConfig config_;
    drogon::orm::DbClientPtr database_;
};

} // namespace launcher::app
