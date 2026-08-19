#include "app/JsonBody.h"

#include "common/Error.h"
#include "domain/Validation.h"

namespace launcher::app {
namespace {

using common::ApiException;
using common::ErrorCode;

} // namespace

Json::Value requireJsonObject(const drogon::HttpRequestPtr& request) {
    const auto body = request->getJsonObject();
    if (body == nullptr) {
        throw ApiException(ErrorCode::InvalidInput,
                           "the request body must be a JSON object with content-type "
                           "application/json");
    }
    if (!body->isObject()) {
        throw ApiException(ErrorCode::InvalidInput, "the request body must be a JSON object");
    }
    return *body;
}

Json::Value optionalJsonObject(const drogon::HttpRequestPtr& request) {
    if (request->body().empty()) {
        return Json::Value(Json::objectValue);
    }
    return requireJsonObject(request);
}

std::string requireString(const Json::Value& body, const char* field, const char* rule) {
    // Absent, of the wrong type and blank are one refusal to whoever is looking at the form:
    // the box is empty. They stay three sentences in the log and are one rule on the wire.
    const std::string named = rule == nullptr ? std::string{} : std::string(rule);

    if (!body.isMember(field) || !body[field].isString()) {
        throw ApiException(common::Error{ErrorCode::InvalidInput,
                                         std::string(field) + " is required and must be a string",
                                         named});
    }

    std::string value = body[field].asString();
    if (domain::trim(value).empty()) {
        throw ApiException(common::Error{
            ErrorCode::InvalidInput, std::string(field) + " must not be empty", named});
    }
    return value;
}

std::string
optionalString(const Json::Value& body, const char* field, const std::string& fallback) {
    if (!body.isMember(field) || !body[field].isString()) {
        return fallback;
    }
    return body[field].asString();
}

int64_t requireInt64(const Json::Value& body, const char* field) {
    if (!body.isMember(field) || !body[field].isNumeric()) {
        throw ApiException(ErrorCode::InvalidInput,
                           std::string(field) + " is required and must be a number");
    }
    if (!body[field].isIntegral() || !body[field].isInt64()) {
        throw ApiException(ErrorCode::InvalidInput,
                           std::string(field) + " must be a whole number that fits in 64 bits");
    }
    return body[field].asInt64();
}

bool optionalBool(const Json::Value& body, const char* field, bool fallback) {
    if (!body.isMember(field) || !body[field].isBool()) {
        return fallback;
    }
    return body[field].asBool();
}

} // namespace launcher::app
