#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <string>
#include <vector>

#include "domain/Actor.h"
#include "domain/Catalog.h"
#include "repositories/IGameRepository.h"

namespace launcher::controllers::v1 {

/// The actor behind a request, built from the claims JwtAuthFilter published. Throws when the
/// route is missing its authentication filter, which is a wiring bug rather than a runtime
/// condition.
domain::Actor actorOf(const drogon::HttpRequestPtr& request);

drogon::HttpResponsePtr jsonResponse(const drogon::HttpRequestPtr& request,
                                     const Json::Value& body,
                                     drogon::HttpStatusCode status = drogon::k200OK);

Json::Value gameToJson(const domain::Game& game);

Json::Value mediaToJson(const domain::GameMedia& media);

Json::Value versionToJson(const domain::GameVersion& version);

Json::Value buildToJson(const domain::Build& build);

Json::Value gameDetailToJson(const domain::GameDetail& detail);

Json::Value gamePageToJson(const repositories::GamePage& page, int limit, int offset);

/// Reads `search`, `sort`, `page` and `pageSize` from the query string. Unknown sort keys fall
/// back to the default rather than failing: a stale client should still get a catalog.
repositories::GameQuery gameQueryOf(const drogon::HttpRequestPtr& request);

} // namespace launcher::controllers::v1
