#include "app/Bootstrap.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/utils/coroutine.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <exception>

#include "common/Logging.h"
#include "services/UploadService.h"

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "app/SecurityHeaders.h"
#include "common/Error.h"
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

void scheduleBlobCollector(uint32_t intervalSeconds) {
    // build_files is ON DELETE RESTRICT, so a referenced blob was never at risk. This is the
    // other half: an upload that was never finalised, and the content of a build somebody
    // deleted, are otherwise paid for and stored forever.
    drogon::app().getLoop()->runEvery(static_cast<double>(intervalSeconds), []() {
        drogon::async_run([]() -> drogon::Task<> {
            try {
                co_await AppContext::instance().retentionService().collectUnreferencedBlobs();
            } catch (const std::exception& e) {
                spdlog::warn("blob sweep failed: {}", common::escapeJson(e.what()));
            }
        });
    });
}

void scheduleCrashReportSweeper(uint32_t intervalSeconds) {
    // Crash reports are the only thing here nobody ever deletes by hand: a launcher sends one
    // and forgets it, and an operator reads a list rather than pruning it. Without this the
    // table grows for the life of the deployment.
    drogon::app().getLoop()->runEvery(static_cast<double>(intervalSeconds), []() {
        drogon::async_run([]() -> drogon::Task<> {
            try {
                co_await AppContext::instance().crashReportService().sweepExpired();
            } catch (const std::exception& e) {
                spdlog::warn("crash report sweep failed: {}", common::escapeJson(e.what()));
            }
        });
    });
}

} // namespace

void configureBodyLimits(const AppConfig& config) {
    // Drogon buffers a request body before the handler ever runs, and its default cap is one
    // megabyte — well under a single upload chunk, so without this every chunk is rejected
    // before reaching the controller.
    //
    // Three budgets, not one. The largest body the server accepts at all is whichever is
    // biggest: an upload chunk, the largest document a route takes — in practice the manifest of
    // a big build — or a video, which arrives whole in one POST because it is one file and not a
    // resumable upload. Until `maxDocumentBytes` existed only the chunk size was here, so how
    // many files a build could contain was a silent consequence of a number about something
    // else; `media.maxVideoBytes` joined it for the same reason, and the failure it prevents is
    // sharper than that one. Drogon refuses an oversized body itself, before any handler runs,
    // with a bare 413 carrying **no** RFC 7807 envelope — measured against the running stack on
    // 2026-08-18 — so a video the framework rejects is a refusal no client can explain to
    // anybody. `media.maxBytes` is deliberately not in the maximum: it is smaller than the chunk
    // on every configuration that makes sense, and folding it in would say otherwise.
    //
    // The *memory* limit stays at the chunk size on purpose: a chunk is read straight back, so
    // spilling it to a temporary file would be pure loss, while the rare manifest that exceeds
    // it — and every video, which is the point — is better on disk than held in RAM once per
    // concurrent request. A spilled body is still readable through `request->getBody()`, which
    // was verified against the running server rather than assumed: a 10 MiB upload with an 8 MiB
    // memory limit reached the handler with its length intact.
    const auto chunk = static_cast<std::size_t>(config.uploads.maxChunkBytes) + BODY_SIZE_HEADROOM;
    const auto largest =
        std::max({chunk,
                  static_cast<std::size_t>(config.server.maxDocumentBytes) + BODY_SIZE_HEADROOM,
                  static_cast<std::size_t>(config.media.maxVideoBytes) + BODY_SIZE_HEADROOM});

    drogon::app().setClientMaxBodySize(largest);
    drogon::app().setClientMaxMemoryBodySize(chunk);

    const auto anonymousLimit = static_cast<std::size_t>(config.server.maxAnonymousBodyBytes);
    drogon::app().registerPreRoutingAdvice([anonymousLimit](const drogon::HttpRequestPtr& request,
                                                            drogon::AdviceCallback&& reject,
                                                            drogon::AdviceChainCallback&& proceed) {
        // Keyed on the absence of a token rather than on a list of routes, so it cannot go
        // stale when a route is added: nothing anonymous on this server has a reason to
        // send a document. A caller can of course attach a token that turns out to be
        // invalid and be measured against the larger limit instead — its route then refuses
        // it — which changes nothing about what was buffered, since the framework read the
        // body before this advice existed to have an opinion.
        if (request->getHeader("Authorization").empty() &&
            request->body().size() > anonymousLimit) {
            reject(makeErrorResponse(
                common::Error{common::ErrorCode::QuotaExceeded,
                              "this request is too large to be sent without an account"},
                requestIdOf(request)));
            return;
        }
        proceed();
    });
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

        registerErrorHandling(config.security);
        registerSecurityHeaders(config.security);

        auto& framework = drogon::app();
        framework.addListener(config.server.listenAddress, config.server.port);
        configureBodyLimits(config);
        scheduleUploadSweeper(config.uploads.sweepIntervalSeconds);
        scheduleBlobCollector(config.retention.sweepIntervalSeconds);
        if (config.crashReports.enabled) {
            scheduleCrashReportSweeper(config.crashReports.sweepIntervalSeconds);
        }

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
