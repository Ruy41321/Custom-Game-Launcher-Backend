#include "controllers/admin/AdminUiController.h"

#include "admin/AdminUi.h"
#include "app/HttpError.h"

namespace launcher::controllers::admin {

void AdminUiController::index(const drogon::HttpRequestPtr& request,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setContentTypeCode(drogon::CT_TEXT_HTML);
    response->setBody(::launcher::admin::INDEX_HTML);

    // The page is entirely self-contained, so the policy that describes it is also the
    // strictest one there is: no origin may be fetched from, and nothing may frame it. That
    // costs nothing here and means a future edit that reaches for a CDN fails loudly in the
    // browser rather than quietly adding a third party to an operator console.
    response->addHeader("Content-Security-Policy",
                        "default-src 'none'; script-src 'unsafe-inline'; style-src "
                        "'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; "
                        "form-action 'none'; base-uri 'none'");
    response->addHeader("X-Content-Type-Options", "nosniff");
    response->addHeader("Referrer-Policy", "no-referrer");
    // Nothing here should ever be reused: an operator returning after a deployment must not be
    // handed a console built against a different API.
    response->addHeader("Cache-Control", "no-store");

    if (const auto requestId = app::requestIdOf(request); !requestId.empty()) {
        response->addHeader("X-Request-Id", requestId);
    }
    callback(response);
}

} // namespace launcher::controllers::admin
