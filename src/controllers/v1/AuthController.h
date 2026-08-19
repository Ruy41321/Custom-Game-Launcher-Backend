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
    // Only the mail bucket, because this one costs no Argon2id hash — it costs a message in
    // somebody's inbox, which is what that bucket is for.
    ADD_METHOD_TO(AuthController::resendVerification,
                  "/api/v1/auth/verify-email/resend",
                  drogon::Post,
                  "launcher::filters::MailRateLimitFilter");
    // Both buckets: it is an unauthenticated credential-adjacent endpoint *and* it sends a
    // message, and the two limits exist for different reasons.
    ADD_METHOD_TO(AuthController::requestPasswordReset,
                  "/api/v1/auth/password-reset/request",
                  drogon::Post,
                  "launcher::filters::AuthRateLimitFilter",
                  "launcher::filters::MailRateLimitFilter");
    ADD_METHOD_TO(AuthController::confirmPasswordReset,
                  "/api/v1/auth/password-reset/confirm",
                  drogon::Post,
                  "launcher::filters::AuthRateLimitFilter");
    // On `/me` rather than under `/auth`, because it acts on the signed-in account — and
    // registered on *this* controller because the answer is a session document, which is
    // built here. `filters::PASSWORD_CHANGE_PATH` names the same path: a session holding an
    // operator's one-time password reaches this route and nothing else, so the two have to
    // agree, and the constant is what makes that checkable.
    //
    // It carries the auth rate-limit bucket for D46's reason: every attempt costs an Argon2id
    // verification and then a hash, which is the most expensive thing an unattended caller
    // can ask this server to do.
    ADD_METHOD_TO(AuthController::changePassword,
                  "/api/v1/me/password",
                  drogon::Post,
                  "launcher::filters::AuthRateLimitFilter",
                  "launcher::filters::JwtAuthFilter");
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

    drogon::Task<> resendVerification(drogon::HttpRequestPtr request,
                                      std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<>
    requestPasswordReset(drogon::HttpRequestPtr request,
                         std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<>
    confirmPasswordReset(drogon::HttpRequestPtr request,
                         std::function<void(const drogon::HttpResponsePtr&)> callback);

    /// Replaces the password of the signed-in account and answers with a fresh session.
    drogon::Task<> changePassword(drogon::HttpRequestPtr request,
                                  std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<> currentUser(drogon::HttpRequestPtr request,
                               std::function<void(const drogon::HttpResponsePtr&)> callback);
};

} // namespace launcher::controllers::v1
