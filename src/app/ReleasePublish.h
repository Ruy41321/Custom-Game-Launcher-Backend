#pragma once

#include <filesystem>
#include <string>

#include "app/Config.h"
#include "common/Result.h"
#include "domain/LauncherRelease.h"

namespace launcher::app {

/// Publishing a launcher release, from the command line, against a document signed somewhere
/// else.
///
/// **The private key is never here.** The operator builds the artifact on their own machine,
/// writes the release document, signs it there, and hands this server two files and a signature
/// it can only ever *check*. That is the property the whole feature is built around: somebody
/// who takes this VPS, its database and its disks can stop launchers from updating, and cannot
/// make them update to anything. No other surface in this repository survives a full compromise,
/// and this is the one where it matters, because an automatic update is code a machine runs
/// without anybody looking at it.
///
/// It is a command rather than an endpoint for the reason `--grant-role` is (D38): the authority
/// that fits is shell access to the machine. Two consequences fall out and both are wanted —
/// the artifact never has to fit inside an HTTP body limit, and there is no route through which
/// a stolen token could ever create a release.
///
/// Like the migration runner and the role grant, this talks to libpq directly: there is no
/// event loop to run a coroutine on, and destroying a Drogon `DbClient` can land on its own
/// connection thread and abort the process.
struct ReleasePublishRequest {
    std::filesystem::path documentPath;
    std::filesystem::path signaturePath;
    std::filesystem::path artifactPath;
};

enum class ReleasePublishOutcome {
    Published,
    /// This exact channel/platform/architecture/version already exists. Not an error the second
    /// time by accident: re-publishing a version is how a mistake gets covered up, and every
    /// client refuses a version that is not strictly newer anyway, so a corrected build has to
    /// carry a new number to reach anybody.
    AlreadyPublished,
};

common::Result<ReleasePublishOutcome> publishRelease(const AppConfig& config,
                                                     const ReleasePublishRequest& request);

/// Runs the publish and reports it on stdout. Returns a process exit code.
int runReleasePublish(const AppConfig& config, const ReleasePublishRequest& request);

enum class ReleaseRetireOutcome {
    Retired,
    /// Either no such release, or one already withdrawn. One outcome rather than two, because
    /// the end state an operator asked for is the same in both.
    NotLive,
};

/// Withdraws a release. The row stays — it is the record of what was once handed out — and the
/// artifact stays on disk, because a launcher that started the download before the withdrawal
/// is better off finishing it than failing halfway. What changes is that the route stops
/// offering it, so clients fall back to the previous release and then decline it for being
/// older than what they are running. **Standing still is the correct outcome of withdrawing a
/// bad build**: rolling a fleet backwards is a bigger action than the one being asked for.
common::Result<ReleaseRetireOutcome> retireRelease(const AppConfig& config,
                                                   const domain::ReleaseQuery& query,
                                                   const std::string& version);

int runReleaseRetire(const AppConfig& config,
                     const domain::ReleaseQuery& query,
                     const std::string& version);

} // namespace launcher::app
