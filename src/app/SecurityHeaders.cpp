#include "app/SecurityHeaders.h"

#include <drogon/HttpAppFramework.h>

#include <string>

namespace launcher::app {
namespace {

void addIfAbsent(const drogon::HttpResponsePtr& response, const char* name, std::string value) {
    if (response->getHeader(name).empty()) {
        response->addHeader(name, std::move(value));
    }
}

} // namespace

void applySecurityHeaders(const drogon::HttpResponsePtr& response, const SecurityConfig& config) {
    // The one header here that is not advice to a browser: it tells any client that the bytes
    // are what the Content-Type says, and it is the reason a JSON error envelope cannot be
    // talked into being rendered as a document.
    addIfAbsent(response, "X-Content-Type-Options", "nosniff");

    // Nothing this API serves is a page, so the honest policy is that nothing may load it as
    // one. `frame-ancestors` is what actually enforces the framing rule in a current browser;
    // X-Frame-Options is kept for the ones that only understand that.
    addIfAbsent(response, "Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'");
    addIfAbsent(response, "X-Frame-Options", "DENY");

    // A signed download URL travels as a URL, and a referrer header is how a URL ends up in
    // somebody else's logs.
    addIfAbsent(response, "Referrer-Policy", "no-referrer");

    if (config.hsts) {
        addIfAbsent(response,
                    "Strict-Transport-Security",
                    "max-age=" + std::to_string(config.hstsMaxAgeSeconds));
    }
}

void registerSecurityHeaders(const SecurityConfig& config) {
    drogon::app().registerPostHandlingAdvice(
        [config](const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr& response) {
            applySecurityHeaders(response, config);
        });
}

} // namespace launcher::app
