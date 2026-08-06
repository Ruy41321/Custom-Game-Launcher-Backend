#include "controllers/v1/AuthController.h"

#include <json/json.h>

#include <utility>

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "app/JsonBody.h"
#include "filters/AuthRateLimitFilter.h"
#include "filters/JwtAuthFilter.h"
#include "services/AuthService.h"

namespace launcher::controllers::v1 {
namespace {

using common::ApiException;
using common::ErrorCode;

Json::Value userToJson(const domain::User& user) {
    Json::Value json;
    json["id"] = user.id;
    json["email"] = user.email;
    json["displayName"] = user.displayName;
    json["emailVerified"] = user.emailVerified;
    json["uploadQuotaBytes"] = static_cast<Json::Int64>(user.uploadQuotaBytes);
    json["uploadUsedBytes"] = static_cast<Json::Int64>(user.uploadUsedBytes);
    return json;
}

Json::Value permissionsToJson(const std::vector<std::string>& permissions) {
    Json::Value json(Json::arrayValue);
    for (const auto& permission : permissions) {
        json.append(permission);
    }
    return json;
}

Json::Value sessionToJson(const services::AuthTokens& tokens) {
    Json::Value json;
    json["accessToken"] = tokens.accessToken;
    json["refreshToken"] = tokens.refreshToken;
    json["tokenType"] = "Bearer";
    json["expiresIn"] = static_cast<Json::Int64>(tokens.accessTokenExpiresIn.count());
    json["user"] = userToJson(tokens.user);
    json["permissions"] = permissionsToJson(tokens.permissions);
    return json;
}

drogon::HttpResponsePtr jsonResponse(const drogon::HttpRequestPtr& request,
                                     const Json::Value& body,
                                     drogon::HttpStatusCode status = drogon::k200OK) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(status);
    if (const auto requestId = app::requestIdOf(request); !requestId.empty()) {
        response->addHeader("X-Request-Id", requestId);
    }
    return response;
}

services::ClientContext clientContextOf(const drogon::HttpRequestPtr& request) {
    return services::ClientContext{request->getHeader("User-Agent"),
                                   filters::clientAddressOf(request)};
}

/// Rendered as an error envelope by the central handler.
[[noreturn]] void fail(const common::Error& error) {
    throw ApiException(error);
}

} // namespace

drogon::Task<>
AuthController::registerUser(drogon::HttpRequestPtr request,
                             std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto body = app::requireJsonObject(request);

    services::RegisterCommand command;
    command.email = app::requireString(body, "email");
    command.password = app::requireString(body, "password");
    command.displayName = app::requireString(body, "displayName");

    auto result =
        co_await app::AppContext::instance().authService().registerUser(std::move(command));
    if (!result.ok()) {
        fail(result.error());
    }

    const auto& context = app::AppContext::instance();
    const auto registration = std::move(result).value();

    Json::Value response;
    response["user"] = userToJson(registration.user);
    response["emailVerificationRequired"] = context.config().auth.requireVerifiedEmail;
    // Said out loud rather than assumed. The account exists either way, and a client that told
    // somebody to check their inbox when the message never left would be describing a wait
    // that never ends; with this it can offer the resend route instead.
    response["verificationEmailSent"] = registration.verificationEmailSent;

    callback(jsonResponse(request, response, drogon::k201Created));
    co_return;
}

drogon::Task<> AuthController::login(drogon::HttpRequestPtr request,
                                     std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto body = app::requireJsonObject(request);
    const auto email = app::requireString(body, "email");
    const auto password = app::requireString(body, "password");

    auto result = co_await app::AppContext::instance().authService().login(
        email, password, clientContextOf(request));
    if (!result.ok()) {
        fail(result.error());
    }

    callback(jsonResponse(request, sessionToJson(std::move(result).value())));
    co_return;
}

drogon::Task<>
AuthController::refresh(drogon::HttpRequestPtr request,
                        std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto body = app::requireJsonObject(request);
    const auto refreshToken = app::requireString(body, "refreshToken");

    auto result = co_await app::AppContext::instance().authService().refresh(
        refreshToken, clientContextOf(request));
    if (!result.ok()) {
        fail(result.error());
    }

    callback(jsonResponse(request, sessionToJson(std::move(result).value())));
    co_return;
}

drogon::Task<>
AuthController::logout(drogon::HttpRequestPtr request,
                       std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto body = app::requireJsonObject(request);
    const auto refreshToken = app::requireString(body, "refreshToken");

    co_await app::AppContext::instance().authService().logout(refreshToken);

    Json::Value response;
    response["status"] = "signed out";
    callback(jsonResponse(request, response));
    co_return;
}

drogon::Task<>
AuthController::verifyEmail(drogon::HttpRequestPtr request,
                            std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto body = app::requireJsonObject(request);
    const auto token = app::requireString(body, "token");

    const auto result = co_await app::AppContext::instance().authService().verifyEmail(token);
    if (!result.ok()) {
        fail(result.error());
    }

    Json::Value response;
    response["status"] = "verified";
    callback(jsonResponse(request, response));
    co_return;
}

drogon::Task<>
AuthController::requestPasswordReset(drogon::HttpRequestPtr request,
                                     std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto body = app::requireJsonObject(request);
    const auto email = app::requireString(body, "email");

    const auto result =
        co_await app::AppContext::instance().authService().requestPasswordReset(email);
    if (!result.ok()) {
        fail(result.error());
    }

    // Always the same answer, whether or not the address exists and whether or not the message
    // could be delivered: this endpoint is unauthenticated, and a distinguishable response
    // makes it an enumeration tool.
    Json::Value response;
    response["status"] = "if that address is registered, a reset link has been sent";

    callback(jsonResponse(request, response));
    co_return;
}

drogon::Task<>
AuthController::resendVerification(drogon::HttpRequestPtr request,
                                   std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto body = app::requireJsonObject(request);
    const auto email = app::requireString(body, "email");

    const auto result =
        co_await app::AppContext::instance().authService().resendVerification(email);
    if (!result.ok()) {
        fail(result.error());
    }

    // One sentence for an unknown address, an already confirmed one and a message just sent,
    // for the same reason the reset request has one.
    Json::Value response;
    response["status"] = "if that address needs confirming, a new link has been sent";

    callback(jsonResponse(request, response));
    co_return;
}

drogon::Task<>
AuthController::confirmPasswordReset(drogon::HttpRequestPtr request,
                                     std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto body = app::requireJsonObject(request);
    const auto token = app::requireString(body, "token");
    const auto password = app::requireString(body, "password");

    const auto result =
        co_await app::AppContext::instance().authService().resetPassword(token, password);
    if (!result.ok()) {
        fail(result.error());
    }

    Json::Value response;
    response["status"] = "password updated";
    callback(jsonResponse(request, response));
    co_return;
}

drogon::Task<>
AuthController::currentUser(drogon::HttpRequestPtr request,
                            std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto& claims = filters::requireClaims(request);

    Json::Value response;
    response["id"] = claims.userId;
    response["email"] = claims.email;
    response["permissions"] = permissionsToJson(claims.permissions);

    callback(jsonResponse(request, response));
    co_return;
}

} // namespace launcher::controllers::v1
