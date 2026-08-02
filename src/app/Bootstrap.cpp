#include "app/Bootstrap.h"

#include <drogon/HttpAppFramework.h>
#include <spdlog/spdlog.h>

#include <exception>

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "common/Logging.h"
#include "launcher/Version.h"
#include "migrations/MigrationRunner.h"

namespace launcher::app {
namespace {

constexpr int EXIT_OK = 0;
constexpr int EXIT_FAILURE_CODE = 1;

} // namespace

drogon::orm::DbClientPtr createDatabaseClient(const DatabaseConfig& config) {
    return drogon::orm::DbClient::newPgClient(config.connectionString(), config.connectionCount);
}

int runMigrations(const AppConfig& config) {
    try {
        migrations::MigrationRunner runner(config.database.connectionString(),
                                           config.migrationsDirectory);

        auto result = runner.run();
        if (!result.ok()) {
            spdlog::error("migration failed: {}", common::escapeJson(result.error().detail));
            return EXIT_FAILURE_CODE;
        }
        return EXIT_OK;
    } catch (const std::exception& e) {
        spdlog::error("migration aborted: {}", common::escapeJson(e.what()));
        return EXIT_FAILURE_CODE;
    }
}

int runServer(const AppConfig& config) {
    try {
        auto database = createDatabaseClient(config.database);
        AppContext::instance().initialize(config, database);

        registerErrorHandling();

        auto& framework = drogon::app();
        framework.addListener(config.server.listenAddress, config.server.port);

        if (config.server.adminEnabled) {
            // Loopback only, by design: the admin surface is reached over an SSH tunnel and
            // is never published. Binding it to 0.0.0.0 would expose privileged endpoints.
            framework.addListener(config.server.adminListenAddress, config.server.adminPort);
            spdlog::info("admin listener enabled on {}:{}",
                         config.server.adminListenAddress,
                         config.server.adminPort);
        }

        if (config.server.threadCount > 0) {
            framework.setThreadNum(config.server.threadCount);
        }
        framework.setLogLevel(trantor::Logger::kWarn);

        spdlog::info("{} {} listening on {}:{} (environment: {})",
                     APP_NAME,
                     APP_VERSION,
                     config.server.listenAddress,
                     config.server.port,
                     config.environment);

        framework.run();
        return EXIT_OK;
    } catch (const std::exception& e) {
        spdlog::critical("server failed to start: {}", common::escapeJson(e.what()));
        return EXIT_FAILURE_CODE;
    }
}

} // namespace launcher::app
