#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

#include <functional>

namespace launcher::controllers::admin {

/// Signing in to the administrative surface.
///
/// A surface of its own rather than a reuse of `/api/v1/auth/login` for a practical reason: an
/// operator reaches this server through `ssh -L 9090:127.0.0.1:9090`, which forwards one port.
/// The page loaded from :9090 therefore has no route to :8080 to obtain a token, so the admin
/// listener has to be able to mint one itself. It is the same AuthService underneath — the
/// difference is that this entry point also requires the account to be an operator, so an
/// ordinary player's correct password does not yield a session here.
class AdminAuthController : public drogon::HttpController<AdminAuthController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminAuthController::login,
                  "/admin/api/auth/login",
                  drogon::Post,
                  "launcher::filters::AdminSurfaceFilter",
                  "launcher::filters::AuthRateLimitFilter");
    ADD_METHOD_TO(AdminAuthController::refresh,
                  "/admin/api/auth/refresh",
                  drogon::Post,
                  "launcher::filters::AdminSurfaceFilter",
                  "launcher::filters::AuthRateLimitFilter");
    ADD_METHOD_TO(AdminAuthController::logout,
                  "/admin/api/auth/logout",
                  drogon::Post,
                  "launcher::filters::AdminSurfaceFilter");
    ADD_METHOD_TO(AdminAuthController::session,
                  "/admin/api/session",
                  drogon::Get,
                  "launcher::filters::AdminSurfaceFilter",
                  "launcher::filters::JwtAuthFilter",
                  "launcher::filters::AdminOperatorFilter");
    METHOD_LIST_END

    drogon::Task<> login(drogon::HttpRequestPtr request,
                         std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<> refresh(drogon::HttpRequestPtr request,
                           std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<> logout(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<> session(drogon::HttpRequestPtr request,
                           std::function<void(const drogon::HttpResponsePtr&)> callback);
};

} // namespace launcher::controllers::admin
