#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

namespace launcher::controllers::v1 {

/// Authentication surface.
///
/// Handlers are coroutines and take their parameters **by value**: a reference parameter to
/// a coroutine dangles the moment it first suspends.
///
/// The unauthenticated endpoints carry AuthRateLimitFilter. Each of them costs an Argon2id
/// hash, so without it they are both a guessing oracle and a cheap CPU-exhaustion vector.
class AuthController : public drogon::HttpController<AuthController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AuthController::registerUser,
                  "/api/v1/auth/register",
                  drogon::Post,
                  "launcher::filters::AuthRateLimitFilter");
    ADD_METHOD_TO(AuthController::login,
                  "/api/v1/auth/login",
                  drogon::Post,
                  "launcher::filters::AuthRateLimitFilter");
    ADD_METHOD_TO(AuthController::refresh, "/api/v1/auth/refresh", drogon::Post);
    ADD_METHOD_TO(AuthController::logout, "/api/v1/auth/logout", drogon::Post);
    ADD_METHOD_TO(AuthController::verifyEmail, "/api/v1/auth/verify-email", drogon::Post);
    ADD_METHOD_TO(AuthController::requestPasswordReset,
                  "/api/v1/auth/password-reset/request",
                  drogon::Post,
                  "launcher::filters::AuthRateLimitFilter");
    ADD_METHOD_TO(AuthController::confirmPasswordReset,
                  "/api/v1/auth/password-reset/confirm",
                  drogon::Post,
                  "launcher::filters::AuthRateLimitFilter");
    ADD_METHOD_TO(AuthController::currentUser,
                  "/api/v1/auth/me",
                  drogon::Get,
                  "launcher::filters::JwtAuthFilter");
    METHOD_LIST_END

    drogon::Task<> registerUser(drogon::HttpRequestPtr request,
                                std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<> login(drogon::HttpRequestPtr request,
                         std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<> refresh(drogon::HttpRequestPtr request,
                           std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<> logout(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<> verifyEmail(drogon::HttpRequestPtr request,
                               std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<>
    requestPasswordReset(drogon::HttpRequestPtr request,
                         std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<>
    confirmPasswordReset(drogon::HttpRequestPtr request,
                         std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<> currentUser(drogon::HttpRequestPtr request,
                               std::function<void(const drogon::HttpResponsePtr&)> callback);
};

} // namespace launcher::controllers::v1
