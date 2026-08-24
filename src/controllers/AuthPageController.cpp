#include "controllers/AuthPageController.h"

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "auth/AuthPages.h"
#include "common/Error.h"

namespace launcher::controllers {
namespace {

drogon::HttpResponsePtr pageResponse(const drogon::HttpRequestPtr& request, const char* html) {
    // A deployment that sends no mail has no links pointing here, and a page telling somebody
    // to open a link they cannot have received is worse than nothing. Same answer as the
    // routes that would send: the feature is not here.
    if (!app::AppContext::instance().config().mail.enabled()) {
        return app::makeErrorResponse(
            common::Error{common::ErrorCode::NotFound, "no such endpoint"},
            app::requestIdOf(request));
    }

    auto response = drogon::HttpResponse::newHttpResponse();
    response->setContentTypeCode(drogon::CT_TEXT_HTML);
    response->setBody(html);

    // The page is self-contained and talks to this origin only, so it can state the strictest
    // policy that still lets it work — and a later edit reaching for a CDN fails loudly in the
    // browser instead of quietly adding a third party to a page people reach from their inbox.
    response->addHeader("Content-Security-Policy",
                        "default-src 'none'; script-src 'unsafe-inline'; style-src "
                        "'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; "
                        "form-action 'none'; base-uri 'none'");
    response->addHeader("X-Content-Type-Options", "nosniff");
    // The URL carries a single-use token, so neither a referrer nor a cache entry may keep it.
    response->addHeader("Referrer-Policy", "no-referrer");
    response->addHeader("Cache-Control", "no-store");

    if (const auto requestId = app::requestIdOf(request); !requestId.empty()) {
        response->addHeader("X-Request-Id", requestId);
    }
    return response;
}

} // namespace

void VerifyEmailPageController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr& request,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    callback(pageResponse(request, ::launcher::auth::VERIFY_EMAIL_HTML));
}

void PasswordResetPageController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr& request,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    callback(pageResponse(request, ::launcher::auth::PASSWORD_RESET_HTML));
}

} // namespace launcher::controllers
