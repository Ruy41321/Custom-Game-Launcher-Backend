#include "controllers/v1/MediaController.h"

#include <json/json.h>

#include <charconv>
#include <utility>

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "app/JsonBody.h"
#include "controllers/v1/CatalogJson.h"
#include "filters/JwtAuthFilter.h"
#include "services/MediaService.h"

namespace launcher::controllers::v1 {
namespace {

using common::ApiException;
using common::ErrorCode;

[[noreturn]] void fail(const common::Error& error) {
    throw ApiException(error);
}

const services::MediaService& media() {
    return app::AppContext::instance().mediaService();
}

constexpr const char* KIND_VALUES = "cover, banner, logo, screenshot";

domain::MediaKind requireKind(const drogon::HttpRequestPtr& request) {
    const auto kind = request->getParameter("kind");
    if (kind.empty()) {
        throw ApiException(ErrorCode::InvalidInput,
                           std::string("a kind query parameter is required, one of: ") +
                               KIND_VALUES);
    }
    const auto parsed = domain::parseMediaKind(kind);
    if (!parsed.has_value()) {
        throw ApiException(ErrorCode::InvalidInput,
                           std::string("kind must be one of: ") + KIND_VALUES);
    }
    return *parsed;
}

int optionalSortOrder(const drogon::HttpRequestPtr& request) {
    const auto text = request->getParameter("sortOrder");
    if (text.empty()) {
        return 0;
    }
    int value = 0;
    const auto* const end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw ApiException(ErrorCode::InvalidInput, "sortOrder must be a whole number");
    }
    return value;
}

} // namespace

drogon::Task<>
MediaController::listForGame(drogon::HttpRequestPtr request,
                             std::function<void(const drogon::HttpResponsePtr&)> callback,
                             std::string idOrSlug) {
    auto listing = co_await media().listForGame(actorOf(request), std::move(idOrSlug));
    if (!listing.ok()) {
        fail(listing.error());
    }

    Json::Value items(Json::arrayValue);
    for (const auto& item : listing.value()) {
        items.append(mediaToJson(item));
    }

    Json::Value body;
    body["items"] = items;
    callback(jsonResponse(request, body));
    co_return;
}

drogon::Task<> MediaController::upload(drogon::HttpRequestPtr request,
                                       std::function<void(const drogon::HttpResponsePtr&)> callback,
                                       std::string idOrSlug) {
    services::UploadMediaCommand command;
    command.kind = requireKind(request);
    command.altText = request->getParameter("altText");
    command.sortOrder = optionalSortOrder(request);

    // The body is the image, byte for byte. Nothing about the request's Content-Type is
    // consulted: the service decides what this is by looking at the bytes.
    const auto body = request->getBody();
    command.bytes.assign(body.data(), body.size());

    auto created =
        co_await media().upload(actorOf(request), std::move(idOrSlug), std::move(command));
    if (!created.ok()) {
        fail(created.error());
    }

    callback(jsonResponse(request, mediaToJson(created.value()), drogon::k201Created));
    co_return;
}

drogon::Task<> MediaController::update(drogon::HttpRequestPtr request,
                                       std::function<void(const drogon::HttpResponsePtr&)> callback,
                                       std::string mediaId) {
    const auto body = app::requireJsonObject(request);

    domain::GameMediaUpdate changes;
    if (body.isMember("altText")) {
        changes.altText = app::optionalString(body, "altText");
    }
    if (body.isMember("sortOrder")) {
        changes.sortOrder = static_cast<int>(app::requireInt64(body, "sortOrder"));
    }

    auto updated =
        co_await media().update(actorOf(request), std::move(mediaId), std::move(changes));
    if (!updated.ok()) {
        fail(updated.error());
    }

    callback(jsonResponse(request, mediaToJson(updated.value())));
    co_return;
}

drogon::Task<> MediaController::remove(drogon::HttpRequestPtr request,
                                       std::function<void(const drogon::HttpResponsePtr&)> callback,
                                       std::string mediaId) {
    auto removed = co_await media().remove(actorOf(request), std::move(mediaId));
    if (!removed.ok()) {
        fail(removed.error());
    }

    callback(noContentResponse(request));
    co_return;
}

} // namespace launcher::controllers::v1
