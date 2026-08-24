#pragma once

#include <drogon/orm/DbClient.h>

#include "app/Config.h"
#include "common/Result.h"

namespace launcher::app {

/// Creates the shared PostgreSQL client. Connection failures surface lazily on first use,
/// which is why readiness is a separate probe from liveness.
drogon::orm::DbClientPtr createDatabaseClient(const DatabaseConfig& config);

/// Applies pending migrations and returns a process exit code.
int runMigrations(const AppConfig& config);

/// Raises Drogon's request-body limits to fit one upload chunk.
///
/// Exposed rather than hidden inside runServer because the integration harness starts the
/// framework itself: if the tests ran against the default one-megabyte cap while production
/// did not, the upload path would be tested under limits nobody deploys.
/// Sets the framework's body limits from the two budgets that describe them, and installs the
/// tighter cap that applies to a request carrying no bearer token. Called by the server and by
/// the integration harness alike, so tests never run under limits nobody deploys.
void configureBodyLimits(const AppConfig& config);

/// Boots the HTTP server. Blocks until the framework is asked to quit.
int runServer(const AppConfig& config);

} // namespace launcher::app
