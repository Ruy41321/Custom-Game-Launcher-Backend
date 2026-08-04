#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <string>
#include <vector>

#include "domain/Actor.h"
#include "domain/Role.h"
#include "repositories/IAdminUserRepository.h"
#include "repositories/IAnalyticsRepository.h"
#include "repositories/IAuditRepository.h"
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

Json::Value userToJson(const repositories::AdminUserSummary& summary);

Json::Value userPageToJson(const repositories::AdminUserPage& page, int limit, int offset);

Json::Value roleToJson(const domain::Role& role);

/// `metadata` is stored as jsonb and travels as a real object, not as a string of JSON: it is
/// the one field whose shape varies by action, and quoting it would push the parsing onto the
/// page for no reason.
Json::Value auditEntryToJson(const domain::AuditEntry& entry);

Json::Value auditPageToJson(const repositories::AuditPage& page, int limit, int offset);

Json::Value downloadReportToJson(const repositories::DownloadReport& report);

/// Reads `search`, `inactive`, `page` and `pageSize` from the query string.
repositories::AdminUserQuery userQueryOf(const drogon::HttpRequestPtr& request);

/// Reads `actor`, `action`, `entityType`, `entityId`, `page` and `pageSize`.
repositories::AuditQuery auditQueryOf(const drogon::HttpRequestPtr& request);

} // namespace launcher::controllers::admin
