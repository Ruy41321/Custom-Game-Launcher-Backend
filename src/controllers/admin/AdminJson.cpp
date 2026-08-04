#include "controllers/admin/AdminJson.h"

#include "app/HttpError.h"
#include "filters/JwtAuthFilter.h"

namespace launcher::controllers::admin {

domain::Actor actorOf(const drogon::HttpRequestPtr& request) {
    const auto& claims = filters::requireClaims(request);
    return domain::Actor{claims.userId, claims.permissions};
}

drogon::HttpResponsePtr jsonResponse(const drogon::HttpRequestPtr& request,
                                     const Json::Value& body,
                                     drogon::HttpStatusCode status) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(status);
    if (const auto requestId = app::requestIdOf(request); !requestId.empty()) {
        response->addHeader("X-Request-Id", requestId);
    }
    return response;
}

drogon::HttpResponsePtr noContentResponse(const drogon::HttpRequestPtr& request) {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k204NoContent);
    if (const auto requestId = app::requestIdOf(request); !requestId.empty()) {
        response->addHeader("X-Request-Id", requestId);
    }
    return response;
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
    json["permissions"] = permissionsToJson(tokens.permissions);

    Json::Value operator_;
    operator_["id"] = tokens.user.id;
    operator_["email"] = tokens.user.email;
    operator_["displayName"] = tokens.user.displayName;
    json["operator"] = operator_;

    return json;
}

} // namespace launcher::controllers::admin
