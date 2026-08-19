#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

#include <functional>
#include <string>

namespace launcher::controllers::admin {

/// Accounts, roles, quotas and the audit trail.
///
/// Every route carries the same three filters in the same order: the listener gate, then
/// authentication, then the operator check. Getting that list wrong is the one mistake this
/// surface cannot survive, which is why none of it is left to the handler bodies.
class AdminUserController : public drogon::HttpController<AdminUserController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminUserController::list,
                  "/admin/api/users",
                  drogon::Get,
                  "launcher::filters::AdminSurfaceFilter",
                  "launcher::filters::JwtAuthFilter",
                  "launcher::filters::AdminOperatorFilter");
    ADD_METHOD_TO(AdminUserController::detail,
                  "/admin/api/users/{1}",
                  drogon::Get,
                  "launcher::filters::AdminSurfaceFilter",
                  "launcher::filters::JwtAuthFilter",
                  "launcher::filters::AdminOperatorFilter");
    ADD_METHOD_TO(AdminUserController::update,
                  "/admin/api/users/{1}",
                  drogon::Patch,
                  "launcher::filters::AdminSurfaceFilter",
                  "launcher::filters::JwtAuthFilter",
                  "launcher::filters::AdminOperatorFilter");
    // The way back in where no mail transport exists. A POST rather than a PATCH on the
    // account, because it is not an edit of a field: it mints a value that exists only in the
    // response, and a route that answers with a secret should not be one anything replays.
    ADD_METHOD_TO(AdminUserController::setTemporaryPassword,
                  "/admin/api/users/{1}/temporary-password",
                  drogon::Post,
                  "launcher::filters::AdminSurfaceFilter",
                  "launcher::filters::JwtAuthFilter",
                  "launcher::filters::AdminOperatorFilter");
    ADD_METHOD_TO(AdminUserController::grantRole,
                  "/admin/api/users/{1}/roles/{2}",
                  drogon::Put,
                  "launcher::filters::AdminSurfaceFilter",
                  "launcher::filters::JwtAuthFilter",
                  "launcher::filters::AdminOperatorFilter");
    ADD_METHOD_TO(AdminUserController::revokeRole,
                  "/admin/api/users/{1}/roles/{2}",
                  drogon::Delete,
                  "launcher::filters::AdminSurfaceFilter",
                  "launcher::filters::JwtAuthFilter",
                  "launcher::filters::AdminOperatorFilter");
    ADD_METHOD_TO(AdminUserController::listRoles,
                  "/admin/api/roles",
                  drogon::Get,
                  "launcher::filters::AdminSurfaceFilter",
                  "launcher::filters::JwtAuthFilter",
                  "launcher::filters::AdminOperatorFilter");
    ADD_METHOD_TO(AdminUserController::listAudit,
                  "/admin/api/audit",
                  drogon::Get,
                  "launcher::filters::AdminSurfaceFilter",
                  "launcher::filters::JwtAuthFilter",
                  "launcher::filters::AdminOperatorFilter");
    METHOD_LIST_END

    drogon::Task<> list(drogon::HttpRequestPtr request,
                        std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<> detail(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback,
                          std::string userId);

    /// Quota and active flag in one PATCH, because they are the two fields of an account an
    /// operator edits and a console form submits them together.
    drogon::Task<> update(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback,
                          std::string userId);

    /// Gives the account a one-time password and answers with it, once.
    drogon::Task<>
    setTemporaryPassword(drogon::HttpRequestPtr request,
                         std::function<void(const drogon::HttpResponsePtr&)> callback,
                         std::string userId);

    drogon::Task<> grantRole(drogon::HttpRequestPtr request,
                             std::function<void(const drogon::HttpResponsePtr&)> callback,
                             std::string userId,
                             std::string roleKey);

    drogon::Task<> revokeRole(drogon::HttpRequestPtr request,
                              std::function<void(const drogon::HttpResponsePtr&)> callback,
                              std::string userId,
                              std::string roleKey);

    drogon::Task<> listRoles(drogon::HttpRequestPtr request,
                             std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<> listAudit(drogon::HttpRequestPtr request,
                             std::function<void(const drogon::HttpResponsePtr&)> callback);
};

} // namespace launcher::controllers::admin
