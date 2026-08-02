#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <string>

#include "common/Error.h"

namespace launcher::app {

constexpr const char* REQUEST_ID_ATTRIBUTE = "requestId";

/// Returns the id assigned to this request by the pre-routing advice, or an empty string.
std::string requestIdOf(const drogon::HttpRequestPtr& request);

/// Builds the single error envelope used by every endpoint (RFC 7807 flavoured):
/// `{ "type", "title", "status", "detail", "code", "requestId" }`.
/// Controllers must never assemble an error body themselves.
drogon::HttpResponsePtr makeErrorResponse(const common::Error& error, const std::string& requestId);

/// Installs the request-id advice, the central exception handler and the 404/405 pages.
/// Called once during server startup.
void registerErrorHandling();

} // namespace launcher::app
