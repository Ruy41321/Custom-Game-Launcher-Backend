#include "controllers/admin/AdminAuthController.h"

#include <json/json.h>

#include <utility>

#include "app/AppContext.h"
#include "app/JsonBody.h"
#include "common/Error.h"
#include "controllers/admin/AdminJson.h"
#include "domain/Admin.h"
#include "filters/AuthRateLimitFilter.h"
#include "filters/JwtAuthFilter.h"

namespace launcher::controllers::admin {
namespace {

using common::ApiException;
using common::ErrorCode;
using common::Result;

services::ClientContext clientContextOf(const drogon::HttpRequestPtr& request) {
    return services::ClientContext{request->getHeader("User-Agent"),
                                   filters::clientAddressOf(request)};
}

/// Accepts a freshly minted session onto the administrative surface, or refuses it.
///
/// The refusal is not merely a different status code: the credentials were correct, so
/// AuthService has already opened a refresh-token family. Retiring it here keeps a rejected
/// sign-in from leaving behind a live session nobody was given.
drogon::Task<Result<services::AuthTokens>> adoptAdminSession(services::AuthTokens tokens) {
    if (domain::mayUseAdminSurface(domain::Actor{tokens.user.id, tokens.permissions})) {
        co_return Result<services::AuthTokens>::success(std::move(tokens));
    }

    co_await app::AppContext::instance().authService().logout(tokens.refreshToken);
    co_return Result<services::AuthTokens>::failure(ErrorCode::Forbidden,
                                                    "this account is not an administrator");
}

[[noreturn]] void fail(const common::Error& error) {
    throw ApiException(error);
}

} // namespace

drogon::Task<>
AdminAuthController::login(drogon::HttpRequestPtr request,
                           std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto body = app::requireJsonObject(request);
    const auto email = app::requireString(body, "email");
    const auto password = app::requireString(body, "password");

    auto result = co_await app::AppContext::instance().authService().login(
        email, password, clientContextOf(request));
    if (!result.ok()) {
        fail(result.error());
    }

    auto adopted = co_await adoptAdminSession(std::move(result).value());
    if (!adopted.ok()) {
        fail(adopted.error());
    }

    callback(jsonResponse(request, sessionToJson(std::move(adopted).value())));
    co_return;
}

drogon::Task<>
AdminAuthController::refresh(drogon::HttpRequestPtr request,
                             std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto body = app::requireJsonObject(request);
    const auto refreshToken = app::requireString(body, "refreshToken");

    auto result = co_await app::AppContext::instance().authService().refresh(
        refreshToken, clientContextOf(request));
    if (!result.ok()) {
        fail(result.error());
    }

    // Checked again on every rotation, not only at sign-in: an operator whose role is revoked
    // mid-session keeps a valid refresh token, and this is where that session ends.
    auto adopted = co_await adoptAdminSession(std::move(result).value());
    if (!adopted.ok()) {
        fail(adopted.error());
    }

    callback(jsonResponse(request, sessionToJson(std::move(adopted).value())));
    co_return;
}

drogon::Task<>
AdminAuthController::logout(drogon::HttpRequestPtr request,
                            std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto body = app::requireJsonObject(request);
    const auto refreshToken = app::requireString(body, "refreshToken");

    co_await app::AppContext::instance().authService().logout(refreshToken);

    callback(noContentResponse(request));
    co_return;
}

drogon::Task<>
AdminAuthController::session(drogon::HttpRequestPtr request,
                             std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto& claims = filters::requireClaims(request);

    Json::Value response;
    response["id"] = claims.userId;
    response["email"] = claims.email;
    response["permissions"] = permissionsToJson(claims.permissions);

    callback(jsonResponse(request, response));
    co_return;
}

} // namespace launcher::controllers::admin
