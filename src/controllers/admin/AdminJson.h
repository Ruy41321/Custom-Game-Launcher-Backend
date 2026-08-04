#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <string>
#include <vector>

#include "domain/Actor.h"
#include "services/AuthService.h"

namespace launcher::controllers::admin {

/// The actor behind an administrative request, built from the claims JwtAuthFilter published.
domain::Actor actorOf(const drogon::HttpRequestPtr& request);

drogon::HttpResponsePtr jsonResponse(const drogon::HttpRequestPtr& request,
                                     const Json::Value& body,
                                     drogon::HttpStatusCode status = drogon::k200OK);

drogon::HttpResponsePtr noContentResponse(const drogon::HttpRequestPtr& request);

Json::Value permissionsToJson(const std::vector<std::string>& permissions);

/// An administrative session. Shaped like the public one so the page can reuse the same
/// handling, minus the fields an operator console has no use for.
Json::Value sessionToJson(const services::AuthTokens& tokens);

} // namespace launcher::controllers::admin
