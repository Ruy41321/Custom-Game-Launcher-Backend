#pragma once

#include <drogon/utils/coroutine.h>

#include <optional>

#include "domain/LauncherRelease.h"

namespace launcher::repositories {

/// Reading launcher releases. There is deliberately no write method here.
///
/// A release is published from the command line by `launcher-api --publish-release`, which
/// talks to libpq directly for the two reasons the migration runner and `--grant-role` do:
/// there is no event loop to run a coroutine on, and destroying a Drogon `DbClient` can land on
/// its own connection thread and abort the process. Leaving the write out of this interface is
/// not an omission — it is what keeps the serving path unable to create a release, which is the
/// same shape as the private key living somewhere this server cannot reach.
class ILauncherReleaseRepository {
  public:
    virtual ~ILauncherReleaseRepository() = default;

    /// The newest live release for one channel/platform/architecture triple, or nothing.
    ///
    /// Nothing is the ordinary answer, not an error: a deployment that has never published a
    /// launcher, and one that has published none for this platform yet, both look the same from
    /// outside — which is what a client asking before there is anything to give it should see.
    virtual drogon::Task<std::optional<domain::LauncherRelease>>
    findLatest(domain::ReleaseQuery query) const = 0;
};

} // namespace launcher::repositories
