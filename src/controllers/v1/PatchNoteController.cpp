#include "controllers/v1/PatchNoteController.h"

#include <json/json.h>

#include <algorithm>
#include <charconv>
#include <utility>

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "app/JsonBody.h"
#include "controllers/v1/CatalogJson.h"
#include "domain/ValidationRules.h"
#include "filters/JwtAuthFilter.h"
#include "services/PatchNoteService.h"

namespace launcher::controllers::v1 {
namespace {

using common::ApiException;

[[noreturn]] void fail(const common::Error& error) {
    throw ApiException(error);
}

const services::PatchNoteService& notes() {
    return app::AppContext::instance().patchNoteService();
}

int parameterAsInt(const drogon::HttpRequestPtr& request, const char* name, int fallback) {
    const auto text = request->getParameter(name);
    if (text.empty()) {
        return fallback;
    }
    int value = fallback;
    const auto* const end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    // A stale or hand-typed query string should still get a devlog, so an unreadable page
    // number falls back rather than failing — the same rule the catalog listing follows.
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return fallback;
    }
    return value;
}

} // namespace

drogon::Task<>
PatchNoteController::listForGame(drogon::HttpRequestPtr request,
                                 std::function<void(const drogon::HttpResponsePtr&)> callback,
                                 std::string idOrSlug) {
    const int limit =
        std::clamp(parameterAsInt(request, "pageSize", repositories::DEFAULT_PATCH_NOTE_PAGE_SIZE),
                   1,
                   repositories::MAX_PATCH_NOTE_PAGE_SIZE);
    const int page = std::max(parameterAsInt(request, "page", 1), 1);
    const int offset = (page - 1) * limit;

    auto listing =
        co_await notes().listForGame(actorOf(request), std::move(idOrSlug), limit, offset);
    if (!listing.ok()) {
        fail(listing.error());
    }

    callback(jsonResponse(request, patchNotePageToJson(listing.value(), limit, offset)));
    co_return;
}

drogon::Task<>
PatchNoteController::create(drogon::HttpRequestPtr request,
                            std::function<void(const drogon::HttpResponsePtr&)> callback,
                            std::string idOrSlug) {
    const auto body = app::requireJsonObject(request);

    services::CreatePatchNoteCommand command;
    command.title = app::requireString(body, "title", domain::rules::PATCH_NOTE_TITLE_REQUIRED);
    command.bodyMarkdown = app::optionalString(body, "bodyMarkdown");
    command.versionId = app::optionalString(body, "versionId");
    command.publish = app::optionalBool(body, "publish");

    auto created =
        co_await notes().create(actorOf(request), std::move(idOrSlug), std::move(command));
    if (!created.ok()) {
        fail(created.error());
    }

    callback(jsonResponse(request, patchNoteToJson(created.value()), drogon::k201Created));
    co_return;
}

drogon::Task<>
PatchNoteController::update(drogon::HttpRequestPtr request,
                            std::function<void(const drogon::HttpResponsePtr&)> callback,
                            std::string noteId) {
    const auto body = app::requireJsonObject(request);

    // Absent means "leave alone", so every field goes through isMember rather than through a
    // default: a PATCH that omits the body must not blank it.
    domain::PatchNoteUpdate changes;
    if (body.isMember("title")) {
        changes.title = app::requireString(body, "title", domain::rules::PATCH_NOTE_TITLE_REQUIRED);
    }
    if (body.isMember("bodyMarkdown")) {
        changes.bodyMarkdown = app::optionalString(body, "bodyMarkdown");
    }
    if (body.isMember("versionId")) {
        changes.gameVersionId = app::optionalString(body, "versionId");
    }
    if (body.isMember("published")) {
        changes.published = app::optionalBool(body, "published");
    }

    auto updated = co_await notes().update(actorOf(request), std::move(noteId), std::move(changes));
    if (!updated.ok()) {
        fail(updated.error());
    }

    callback(jsonResponse(request, patchNoteToJson(updated.value())));
    co_return;
}

drogon::Task<>
PatchNoteController::remove(drogon::HttpRequestPtr request,
                            std::function<void(const drogon::HttpResponsePtr&)> callback,
                            std::string noteId) {
    auto removed = co_await notes().remove(actorOf(request), std::move(noteId));
    if (!removed.ok()) {
        fail(removed.error());
    }

    callback(noContentResponse(request));
    co_return;
}

} // namespace launcher::controllers::v1
