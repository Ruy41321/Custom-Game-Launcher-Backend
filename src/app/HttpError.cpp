#include "app/HttpError.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/utils/Utilities.h>
#include <json/json.h>
#include <spdlog/spdlog.h>

#include "app/SecurityHeaders.h"
#include "common/Logging.h"

namespace launcher::app {
namespace {

using common::Error;
using common::ErrorCode;

/// What `makeErrorResponse` stamps on the envelopes it builds, set once at start-up.
///
/// It is held here rather than passed in because of where these responses come from: a filter
/// that refuses a request answers it itself, and **a response a filter rejects with never
/// reaches post-handling advice**. Every 401, every 403 and every throttle on this server is
/// therefore invisible to the advice that stamps the rest, and this is the one place all of
/// them pass through. The default is the safe one, so an envelope built before start-up
/// finished is still described.
SecurityConfig SECURITY;

} // namespace

std::string requestIdOf(const drogon::HttpRequestPtr& request) {
    const auto& attributes = request->attributes();
    if (!attributes->find(REQUEST_ID_ATTRIBUTE)) {
        return {};
    }
    return attributes->get<std::string>(REQUEST_ID_ATTRIBUTE);
}

drogon::HttpResponsePtr makeErrorResponse(const Error& error, const std::string& requestId) {
    Json::Value body;
    body["type"] = "about:blank";
    body["title"] = titleFor(error.code);
    body["status"] = httpStatusFor(error.code);
    body["code"] = nameFor(error.code);
    body["detail"] = error.detail;
    if (!requestId.empty()) {
        body["requestId"] = requestId;
    }

    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(httpStatusFor(error.code)));
    if (!requestId.empty()) {
        response->addHeader("X-Request-Id", requestId);
    }
    applySecurityHeaders(response, SECURITY);
    return response;
}

void registerErrorHandling(const SecurityConfig& security) {
    SECURITY = security;

    drogon::app().registerPreRoutingAdvice([](const drogon::HttpRequestPtr& request) {
        // An id supplied by an upstream proxy is honoured so a request can be traced across
        // hops; otherwise one is minted here.
        std::string incoming = request->getHeader("X-Request-Id");
        request->attributes()->insert(REQUEST_ID_ATTRIBUTE,
                                      incoming.empty() ? drogon::utils::getUuid()
                                                       : std::move(incoming));
    });

    // Logging only. This advice deliberately does not stamp X-Request-Id on the response:
    // Drogon hands out one shared, cached object for the custom 404 page, and mutating it
    // here would either accumulate headers or pin the first request's id onto every later
    // 404. The header is set where the response is built instead — in makeErrorResponse and
    // in the controllers.
    drogon::app().registerPostHandlingAdvice(
        [](const drogon::HttpRequestPtr& request, const drogon::HttpResponsePtr& response) {
            const auto requestId = requestIdOf(request);
            spdlog::info(R"(request method={} path={} status={} requestId={})",
                         request->methodString(),
                         common::escapeJson(request->path()),
                         static_cast<int>(response->statusCode()),
                         common::escapeJson(requestId));
        });

    drogon::app().setExceptionHandler(
        [](const std::exception& exception,
           const drogon::HttpRequestPtr& request,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            const auto requestId = requestIdOf(request);

            if (const auto* apiException = dynamic_cast<const common::ApiException*>(&exception)) {
                spdlog::warn(R"(handled error code={} detail={} requestId={})",
                             nameFor(apiException->error().code),
                             common::escapeJson(apiException->error().detail),
                             common::escapeJson(requestId));
                callback(makeErrorResponse(apiException->error(), requestId));
                return;
            }

            spdlog::error(R"(unhandled exception detail={} requestId={})",
                          common::escapeJson(exception.what()),
                          common::escapeJson(requestId));

            // Internal failures never leak their detail to the client; the request id is
            // the link between what the user sees and what the logs contain.
            callback(makeErrorResponse(Error{ErrorCode::Internal, "An unexpected error occurred."},
                                       requestId));
        });

    // Built after SECURITY is set, so it carries its headers from `makeErrorResponse` — which
    // for this response is the only safe moment to write them: Drogon caches this object and
    // hands the same one to every request that misses, so a later write to it is a write two
    // event loops can make at once. See applySecurityHeaders.
    drogon::app().setCustom404Page(
        makeErrorResponse(Error{ErrorCode::NotFound, "The requested resource does not exist."}, {}),
        /*set404=*/true);
}

} // namespace launcher::app
