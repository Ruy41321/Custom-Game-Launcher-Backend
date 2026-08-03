#pragma once

#include <drogon/HttpRequest.h>
#include <json/json.h>

#include <cstdint>
#include <string>

namespace launcher::app {

/// Parses the request body as a JSON object, throwing ApiException(InvalidInput) when it is
/// missing, malformed or not an object. Controllers therefore never branch on parse
/// failures; the central exception handler renders them.
Json::Value requireJsonObject(const drogon::HttpRequestPtr& request);

/// Reads a required string field. An absent, non-string or blank value is a validation
/// error naming the field, so the client can point at the right input.
std::string requireString(const Json::Value& body, const char* field);

/// Reads an optional string field, returning the fallback when absent or not a string.
std::string
optionalString(const Json::Value& body, const char* field, const std::string& fallback = {});

/// Reads a required integer field. A JSON number that is not integral — `1.5`, or a value
/// past the 64-bit range — is a validation error rather than a silent truncation.
int64_t requireInt64(const Json::Value& body, const char* field);

bool optionalBool(const Json::Value& body, const char* field, bool fallback = false);

} // namespace launcher::app
