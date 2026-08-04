#include "app/RoleGrant.h"

#include <libpq-fe.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace launcher::app {
namespace {

using common::ErrorCode;
using common::Result;

using ConnectionPtr = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using ResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

constexpr int EXIT_OK = 0;
constexpr int EXIT_FAILURE_CODE = 1;

std::string trimmed(const char* message) {
    std::string text = message == nullptr ? "" : message;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

/// The same statement the administrative endpoint runs, minus the actor: the audit arm selects
/// from the arm that inserted, so a role already held records nothing, and `actor_user_id` is
/// null because a command line is not a user. The metadata says how it happened, so a later
/// reader is not left wondering who the missing actor was.
constexpr const char* GRANT_SQL = R"(
    WITH target AS (SELECT id FROM users WHERE email = $1),
         wanted AS (SELECT id FROM roles WHERE key = $2),
         granted AS (
             INSERT INTO user_roles (user_id, role_id)
             SELECT t.id, w.id FROM target t, wanted w
             ON CONFLICT DO NOTHING
             RETURNING role_id
         ),
         logged AS (
             INSERT INTO audit_log (actor_user_id, action, entity_type, entity_id, metadata)
             SELECT NULL, 'user.role.granted', 'user', (SELECT id::text FROM target),
                    jsonb_build_object('role', $2::text, 'via', 'command-line')
             FROM granted
         )
    SELECT (SELECT count(*) FROM target)  AS user_exists,
           (SELECT count(*) FROM wanted)  AS role_exists,
           (SELECT count(*) FROM granted) AS changed
)";

} // namespace

Result<RoleGrantOutcome> grantRoleFromCommandLine(const AppConfig& config,
                                                  const std::string& email,
                                                  const std::string& roleKey) {
    const ConnectionPtr connection(PQconnectdb(config.database.connectionString().c_str()),
                                   &PQfinish);
    if (PQstatus(connection.get()) != CONNECTION_OK) {
        return Result<RoleGrantOutcome>::failure(ErrorCode::DependencyFailure,
                                                 "cannot connect to the database: " +
                                                     trimmed(PQerrorMessage(connection.get())));
    }

    const char* parameters[] = {email.c_str(), roleKey.c_str()};
    const ResultPtr result(
        PQexecParams(connection.get(), GRANT_SQL, 2, nullptr, parameters, nullptr, nullptr, 0),
        &PQclear);

    if (PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
        return Result<RoleGrantOutcome>::failure(ErrorCode::DependencyFailure,
                                                 "cannot grant the role: " +
                                                     trimmed(PQerrorMessage(connection.get())));
    }

    const std::string userExists = PQgetvalue(result.get(), 0, 0);
    const std::string roleExists = PQgetvalue(result.get(), 0, 1);
    const std::string changed = PQgetvalue(result.get(), 0, 2);

    if (userExists == "0") {
        return Result<RoleGrantOutcome>::success(RoleGrantOutcome::NoSuchUser);
    }
    if (roleExists == "0") {
        return Result<RoleGrantOutcome>::success(RoleGrantOutcome::NoSuchRole);
    }
    return Result<RoleGrantOutcome>::success(changed == "0" ? RoleGrantOutcome::AlreadyHeld
                                                            : RoleGrantOutcome::Granted);
}

int runRoleGrant(const AppConfig& config, const std::string& email, const std::string& roleKey) {
    auto result = grantRoleFromCommandLine(config, email, roleKey);
    if (!result.ok()) {
        std::cerr << result.error().detail << '\n';
        return EXIT_FAILURE_CODE;
    }

    // Reported on stdout rather than through the logger: this is a command somebody typed and
    // is waiting on, not a server event.
    switch (result.value()) {
    case RoleGrantOutcome::Granted:
        std::cout << "granted '" << roleKey << "' to " << email << '\n';
        return EXIT_OK;
    case RoleGrantOutcome::AlreadyHeld:
        // Not a failure. Running it twice is how somebody checks, and the end state is what
        // was asked for either way.
        std::cout << email << " already holds '" << roleKey << "'\n";
        return EXIT_OK;
    case RoleGrantOutcome::NoSuchUser:
        std::cerr << "no account is registered as " << email << '\n';
        return EXIT_FAILURE_CODE;
    case RoleGrantOutcome::NoSuchRole:
        std::cerr << "no such role: " << roleKey << '\n';
        return EXIT_FAILURE_CODE;
    }
    return EXIT_FAILURE_CODE;
}

} // namespace launcher::app
