#include "controllers/admin/AdminJson.h"

#include <algorithm>
#include <charconv>
#include <sstream>

#include "app/HttpError.h"
#include "common/Error.h"
#include "domain/Validation.h"
#include "filters/JwtAuthFilter.h"

namespace launcher::controllers::admin {
namespace {

int parseInt(const std::string& text, int fallback) {
    if (text.empty()) {
        return fallback;
    }
    int value = fallback;
    const auto* const end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return fallback;
    }
    return value;
}

/// Turns `page`/`pageSize` into the limit and offset the repositories take, clamping both. A
/// nonsensical page is answered with the first one rather than an error: a paging control is
/// not worth a failed request.
std::pair<int, int> pagingOf(const drogon::HttpRequestPtr& request, int defaultSize, int maxSize) {
    const int size =
        std::clamp(parseInt(request->getParameter("pageSize"), defaultSize), 1, maxSize);
    const int page = std::max(parseInt(request->getParameter("page"), 1), 1);
    return {size, (page - 1) * size};
}

/// Parses stored jsonb back into a document. A row the database wrote is always valid JSON, so
/// a parse failure here means something other than this code wrote it; an empty object is the
/// honest rendering of "there is nothing readable to show".
Json::Value parseMetadata(const std::string& text) {
    Json::Value parsed;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(text);
    if (!Json::parseFromStream(builder, stream, &parsed, &errors) || !parsed.isObject()) {
        return Json::Value(Json::objectValue);
    }
    return parsed;
}

} // namespace

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

Json::Value userToJson(const repositories::AdminUserSummary& summary) {
    Json::Value json;
    json["id"] = summary.user.id;
    json["email"] = summary.user.email;
    json["displayName"] = summary.user.displayName;
    json["emailVerified"] = summary.user.emailVerified;
    json["active"] = summary.user.isActive;
    json["uploadQuotaBytes"] = static_cast<Json::Int64>(summary.user.uploadQuotaBytes);
    json["uploadUsedBytes"] = static_cast<Json::Int64>(summary.user.uploadUsedBytes);
    json["createdAt"] = summary.createdAt;
    json["lastLoginAt"] = summary.lastLoginAt;
    json["roles"] = permissionsToJson(summary.roles);
    return json;
}

Json::Value userPageToJson(const repositories::AdminUserPage& page, int limit, int offset) {
    Json::Value items(Json::arrayValue);
    for (const auto& summary : page.items) {
        items.append(userToJson(summary));
    }

    Json::Value json;
    json["items"] = items;
    json["total"] = static_cast<Json::Int64>(page.total);
    json["pageSize"] = limit;
    json["page"] = limit > 0 ? (offset / limit) + 1 : 1;
    return json;
}

Json::Value roleToJson(const domain::Role& role) {
    Json::Value json;
    json["key"] = role.key;
    json["description"] = role.description;
    return json;
}

Json::Value auditEntryToJson(const domain::AuditEntry& entry) {
    Json::Value json;
    json["id"] = static_cast<Json::Int64>(entry.id);
    json["actorUserId"] = entry.actorUserId;
    json["actorEmail"] = entry.actorEmail;
    json["action"] = entry.action;
    json["entityType"] = entry.entityType;
    json["entityId"] = entry.entityId;
    json["metadata"] = parseMetadata(entry.metadataJson);
    json["createdAt"] = entry.createdAt;
    return json;
}

Json::Value auditPageToJson(const repositories::AuditPage& page, int limit, int offset) {
    Json::Value items(Json::arrayValue);
    for (const auto& entry : page.items) {
        items.append(auditEntryToJson(entry));
    }

    Json::Value json;
    json["items"] = items;
    json["total"] = static_cast<Json::Int64>(page.total);
    json["pageSize"] = limit;
    json["page"] = limit > 0 ? (offset / limit) + 1 : 1;
    return json;
}

Json::Value downloadReportToJson(const repositories::DownloadReport& report) {
    Json::Value totals;
    totals["downloads"] = static_cast<Json::Int64>(report.totals.downloads);
    totals["distinctUsers"] = static_cast<Json::Int64>(report.totals.distinctUsers);
    totals["bytesPlanned"] = static_cast<Json::Int64>(report.totals.bytesPlanned);
    totals["fullDownloads"] = static_cast<Json::Int64>(report.totals.fullDownloads);
    totals["deltaDownloads"] = static_cast<Json::Int64>(report.totals.deltaDownloads);

    Json::Value daily(Json::arrayValue);
    for (const auto& day : report.daily) {
        Json::Value entry;
        entry["day"] = day.day;
        entry["downloads"] = static_cast<Json::Int64>(day.downloads);
        entry["bytesPlanned"] = static_cast<Json::Int64>(day.bytesPlanned);
        daily.append(entry);
    }

    Json::Value games(Json::arrayValue);
    for (const auto& game : report.topGames) {
        Json::Value entry;
        entry["gameId"] = game.gameId;
        entry["slug"] = game.slug;
        entry["title"] = game.title;
        entry["downloads"] = static_cast<Json::Int64>(game.downloads);
        entry["bytesPlanned"] = static_cast<Json::Int64>(game.bytesPlanned);
        entry["distinctUsers"] = static_cast<Json::Int64>(game.distinctUsers);
        games.append(entry);
    }

    Json::Value json;
    json["days"] = report.days;
    json["totals"] = totals;
    json["daily"] = daily;
    json["topGames"] = games;
    return json;
}

repositories::AdminUserQuery userQueryOf(const drogon::HttpRequestPtr& request) {
    const auto [limit, offset] = pagingOf(request,
                                          repositories::DEFAULT_ADMIN_USER_PAGE_SIZE,
                                          repositories::MAX_ADMIN_USER_PAGE_SIZE);

    repositories::AdminUserQuery query;
    query.search = request->getParameter("search");
    query.onlyInactive = request->getParameter("inactive") == "true";
    query.limit = limit;
    query.offset = offset;
    return query;
}

repositories::AuditQuery auditQueryOf(const drogon::HttpRequestPtr& request) {
    const auto [limit, offset] =
        pagingOf(request, repositories::DEFAULT_AUDIT_PAGE_SIZE, repositories::MAX_AUDIT_PAGE_SIZE);

    repositories::AuditQuery query;
    query.actorUserId = request->getParameter("actor");
    if (!query.actorUserId.empty() && !domain::isUuid(query.actorUserId)) {
        // Refused rather than ignored: quietly dropping an unparseable filter would answer a
        // question nobody asked, and the whole page of results would look like the answer.
        throw common::ApiException(common::ErrorCode::InvalidInput, "actor must be a user id");
    }
    query.action = request->getParameter("action");
    query.entityType = request->getParameter("entityType");
    query.entityId = request->getParameter("entityId");
    query.limit = limit;
    query.offset = offset;
    return query;
}

} // namespace launcher::controllers::admin
