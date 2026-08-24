#include "controllers/admin/AdminUserController.h"

#include <json/json.h>

#include <utility>

#include "app/AppContext.h"
#include "app/JsonBody.h"
#include "common/Error.h"
#include "controllers/admin/AdminJson.h"

namespace launcher::controllers::admin {
namespace {

using common::ApiException;

[[noreturn]] void fail(const common::Error& error) {
    throw ApiException(error);
}

const services::AdminUserService& service() {
    return app::AppContext::instance().adminUserService();
}

} // namespace

drogon::Task<>
AdminUserController::list(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto query = userQueryOf(request);

    auto result = co_await service().list(actorOf(request), query);
    if (!result.ok()) {
        fail(result.error());
    }

    callback(jsonResponse(request,
                          userPageToJson(std::move(result).value(), query.limit, query.offset)));
    co_return;
}

drogon::Task<>
AdminUserController::detail(drogon::HttpRequestPtr request,
                            std::function<void(const drogon::HttpResponsePtr&)> callback,
                            std::string userId) {
    auto result = co_await service().find(actorOf(request), std::move(userId));
    if (!result.ok()) {
        fail(result.error());
    }

    callback(jsonResponse(request, userToJson(std::move(result).value())));
    co_return;
}

drogon::Task<>
AdminUserController::update(drogon::HttpRequestPtr request,
                            std::function<void(const drogon::HttpResponsePtr&)> callback,
                            std::string userId) {
    const auto body = app::requireJsonObject(request);
    const auto actor = actorOf(request);

    // Two independent changes behind one PATCH, each applied only when its field is present.
    // They are separate statements because each carries its own audit entry: "quota raised"
    // and "account disabled" are two things that happened, and a trail that merged them into
    // one row would answer neither question later.
    bool changed = false;
    common::Result<repositories::AdminUserSummary> current =
        common::Result<repositories::AdminUserSummary>::failure(common::ErrorCode::Internal, "");

    if (body.isMember("uploadQuotaBytes")) {
        current = co_await service().setUploadQuota(
            actor, userId, app::requireInt64(body, "uploadQuotaBytes"));
        if (!current.ok()) {
            fail(current.error());
        }
        changed = true;
    }

    if (body.isMember("active")) {
        current = co_await service().setActive(actor, userId, body["active"].asBool());
        if (!current.ok()) {
            fail(current.error());
        }
        changed = true;
    }

    if (!changed) {
        // An empty PATCH is not an error, but it must not silently look like a change either:
        // the account is returned exactly as it stands, and nothing is recorded.
        current = co_await service().find(actor, std::move(userId));
        if (!current.ok()) {
            fail(current.error());
        }
    }

    callback(jsonResponse(request, userToJson(std::move(current).value())));
    co_return;
}

drogon::Task<> AdminUserController::setTemporaryPassword(
    drogon::HttpRequestPtr request,
    std::function<void(const drogon::HttpResponsePtr&)> callback,
    std::string userId) {
    auto result = co_await service().setTemporaryPassword(actorOf(request), std::move(userId));
    if (!result.ok()) {
        fail(result.error());
    }

    auto issued = std::move(result).value();

    Json::Value response;
    response["user"] = userToJson(issued.user);
    // The only place this value ever appears. It is not stored, not logged and not
    // recoverable: an operator who loses it issues another one.
    response["temporaryPassword"] = issued.password;

    callback(jsonResponse(request, response));
    co_return;
}

drogon::Task<>
AdminUserController::grantRole(drogon::HttpRequestPtr request,
                               std::function<void(const drogon::HttpResponsePtr&)> callback,
                               std::string userId,
                               std::string roleKey) {
    auto result =
        co_await service().grantRole(actorOf(request), std::move(userId), std::move(roleKey));
    if (!result.ok()) {
        fail(result.error());
    }

    callback(jsonResponse(request, userToJson(std::move(result).value())));
    co_return;
}

drogon::Task<>
AdminUserController::revokeRole(drogon::HttpRequestPtr request,
                                std::function<void(const drogon::HttpResponsePtr&)> callback,
                                std::string userId,
                                std::string roleKey) {
    auto result =
        co_await service().revokeRole(actorOf(request), std::move(userId), std::move(roleKey));
    if (!result.ok()) {
        fail(result.error());
    }

    callback(jsonResponse(request, userToJson(std::move(result).value())));
    co_return;
}

drogon::Task<>
AdminUserController::listRoles(drogon::HttpRequestPtr request,
                               std::function<void(const drogon::HttpResponsePtr&)> callback) {
    auto result = co_await service().listRoles(actorOf(request));
    if (!result.ok()) {
        fail(result.error());
    }

    Json::Value items(Json::arrayValue);
    for (const auto& role : std::move(result).value()) {
        items.append(roleToJson(role));
    }

    Json::Value response;
    response["items"] = items;
    callback(jsonResponse(request, response));
    co_return;
}

drogon::Task<>
AdminUserController::listAudit(drogon::HttpRequestPtr request,
                               std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto query = auditQueryOf(request);

    auto result = co_await service().listAudit(actorOf(request), query);
    if (!result.ok()) {
        fail(result.error());
    }

    callback(jsonResponse(request,
                          auditPageToJson(std::move(result).value(), query.limit, query.offset)));
    co_return;
}

} // namespace launcher::controllers::admin
