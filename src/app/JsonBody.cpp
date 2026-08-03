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

std::string requireString(const Json::Value& body, const char* field) {
    if (!body.isMember(field) || !body[field].isString()) {
        throw ApiException(ErrorCode::InvalidInput,
                           std::string(field) + " is required and must be a string");
    }

    std::string value = body[field].asString();
    if (domain::trim(value).empty()) {
        throw ApiException(ErrorCode::InvalidInput, std::string(field) + " must not be empty");
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

} // namespace launcher::app
