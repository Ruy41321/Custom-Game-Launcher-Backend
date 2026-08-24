#pragma once

#include <string>

#include "app/Config.h"
#include "common/Result.h"

namespace launcher::app {

enum class RoleGrantOutcome {
    Granted,
    AlreadyHeld,
    NoSuchUser,
    NoSuchRole,
};

/// Grants a role from the command line, without starting the server.
///
/// This exists because the administrative surface cannot bootstrap itself: granting a role
/// there requires `admin.roles.manage`, which on a fresh deployment nobody holds. Something
/// outside the permission system has to hand out the first one, and the only authority that
/// makes sense for it is shell access to the machine — the same authority that already reaches
/// the loopback listener.
///
/// It also retires the manual `INSERT INTO user_roles ...` that the setup notes have carried
/// since the catalog work: the devlist is granted the same way, and gets an audit row for it.
///
/// Talks to libpq directly rather than through Drogon, for the same two reasons the migration
/// runner does: there is no event loop to run a coroutine on, and destroying a DbClient can
/// land on its own connection thread and abort the process.
common::Result<RoleGrantOutcome> grantRoleFromCommandLine(const AppConfig& config,
                                                          const std::string& email,
                                                          const std::string& roleKey);

/// Runs the grant and reports it on stdout. Returns a process exit code.
int runRoleGrant(const AppConfig& config, const std::string& email, const std::string& roleKey);

} // namespace launcher::app
