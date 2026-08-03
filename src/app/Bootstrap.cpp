#include "app/Bootstrap.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/utils/coroutine.h>
#include <spdlog/spdlog.h>

#include <cstddef>
#include <exception>

#include "common/Logging.h"
#include "services/UploadService.h"

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "common/Logging.h"
#include "launcher/Version.h"
#include "migrations/MigrationRunner.h"

namespace launcher::app {
namespace {

constexpr int EXIT_OK = 0;
constexpr int EXIT_FAILURE_CODE = 1;

/// Slack above one upload chunk, for headers and the framing around the body.
constexpr std::size_t BODY_SIZE_HEADROOM = 64U * 1024U;

void scheduleUploadSweeper(uint32_t intervalSeconds) {
    // Abandoned uploads are the only thing holding staging disk that nothing will ever come
    // back for, so they are reclaimed on a timer rather than at request time.
    drogon::app().getLoop()->runEvery(static_cast<double>(intervalSeconds), []() {
        drogon::async_run([]() -> drogon::Task<> {
            try {
                co_await AppContext::instance().uploadService().sweepExpiredSessions();
            } catch (const std::exception& e) {
                spdlog::warn("upload sweep failed: {}", common::escapeJson(e.what()));
            }
        });
    });
}

} // namespace

void configureUploadLimits(const UploadConfig& uploads) {
    // Drogon buffers a request body before the handler ever runs, and its default cap is one
    // megabyte — well under a single upload chunk, so without this every chunk is rejected
    // before reaching the controller. The memory limit is raised alongside it so a chunk is
    // not spilled to a temporary file only to be read straight back.
    const auto limit = static_cast<std::size_t>(uploads.maxChunkBytes) + BODY_SIZE_HEADROOM;
    drogon::app().setClientMaxBodySize(limit);
    drogon::app().setClientMaxMemoryBodySize(limit);
}

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
        configureUploadLimits(config.uploads);
        scheduleUploadSweeper(config.uploads.sweepIntervalSeconds);

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
