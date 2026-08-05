#include "controllers/v1/AccountController.h"

#include <json/json.h>

#include <utility>

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "app/JsonBody.h"
#include "controllers/v1/CatalogJson.h"
#include "filters/JwtAuthFilter.h"
#include "services/AccountService.h"

namespace launcher::controllers::v1 {

drogon::Task<>
AccountController::requestErasure(drogon::HttpRequestPtr request,
                                  std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto body = app::requireJsonObject(request);

    services::EraseAccountCommand command;
    command.password = app::requireString(body, "password");
    command.reason = app::optionalString(body, "reason");

    const auto erased = co_await app::AppContext::instance().accountService().erase(
        actorOf(request), std::move(command));
    if (!erased.ok()) {
        throw common::ApiException(erased.error());
    }

    // Nothing left to describe, and deliberately no farewell document: every field it could
    // carry is something the request just asked to stop existing.
    callback(noContentResponse(request));
    co_return;
}

} // namespace launcher::controllers::v1
