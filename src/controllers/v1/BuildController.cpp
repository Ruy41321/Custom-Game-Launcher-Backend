#include "controllers/v1/BuildController.h"

#include <json/json.h>

#include <utility>
#include <vector>

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "app/JsonBody.h"
#include "controllers/v1/CatalogJson.h"
#include "controllers/v1/UploadJson.h"
#include "filters/JwtAuthFilter.h"
#include "services/UploadService.h"

namespace launcher::controllers::v1 {
namespace {

using common::ApiException;
using common::ErrorCode;

[[noreturn]] void fail(const common::Error& error) {
    throw ApiException(error);
}

const services::UploadService& uploads() {
    return app::AppContext::instance().uploadService();
}

Json::Value requireArray(const Json::Value& body, const char* field) {
    if (!body.isMember(field) || !body[field].isArray()) {
        throw ApiException(ErrorCode::InvalidInput,
                           std::string(field) + " is required and must be an array");
    }
    return body[field];
}

Json::Value requireObjectElement(const Json::Value& element, const char* field) {
    if (!element.isObject()) {
        throw ApiException(ErrorCode::InvalidInput, std::string(field) + " must contain objects");
    }
    return element;
}

std::vector<services::BlobDeclaration> readDeclarations(const Json::Value& body) {
    const auto blobs = requireArray(body, "blobs");

    std::vector<services::BlobDeclaration> declarations;
    declarations.reserve(blobs.size());
    for (const auto& element : blobs) {
        const auto entry = requireObjectElement(element, "blobs");
        declarations.push_back(services::BlobDeclaration{app::requireString(entry, "sha256"),
                                                         app::requireInt64(entry, "size")});
    }
    return declarations;
}

std::vector<domain::ManifestEntry> readManifestFiles(const Json::Value& body) {
    const auto files = requireArray(body, "files");

    std::vector<domain::ManifestEntry> entries;
    entries.reserve(files.size());
    for (const auto& element : files) {
        const auto entry = requireObjectElement(element, "files");

        domain::ManifestEntry file;
        file.relativePath = app::requireString(entry, "path");
        file.blobSha256 = app::requireString(entry, "sha256");
        file.isExecutable = app::optionalBool(entry, "executable");
        entries.push_back(std::move(file));
    }
    return entries;
}

} // namespace

drogon::Task<>
BuildController::missingBlobs(drogon::HttpRequestPtr request,
                              std::function<void(const drogon::HttpResponsePtr&)> callback,
                              std::string buildId) {
    const auto body = app::requireJsonObject(request);

    auto missing = co_await uploads().missingBlobs(
        actorOf(request), std::move(buildId), readDeclarations(body));
    if (!missing.ok()) {
        fail(missing.error());
    }

    Json::Value shas(Json::arrayValue);
    for (const auto& sha256 : missing.value()) {
        shas.append(sha256);
    }

    Json::Value response;
    response["missing"] = shas;
    callback(jsonResponse(request, response));
    co_return;
}

drogon::Task<>
BuildController::beginUpload(drogon::HttpRequestPtr request,
                             std::function<void(const drogon::HttpResponsePtr&)> callback,
                             std::string buildId) {
    const auto body = app::requireJsonObject(request);

    services::BlobDeclaration blob{app::requireString(body, "sha256"),
                                   app::requireInt64(body, "size")};

    auto session =
        co_await uploads().beginUpload(actorOf(request), std::move(buildId), std::move(blob));
    if (!session.ok()) {
        fail(session.error());
    }

    auto response =
        jsonResponse(request, uploadSessionToJson(session.value()), drogon::k201Created);
    // The offset a resumed upload continues at travels as a header too, so a client can act on
    // it without parsing the body.
    response->addHeader("Upload-Offset", std::to_string(session.value().receivedBytes));
    response->addHeader("Location", "/api/v1/uploads/" + session.value().id);
    callback(response);
    co_return;
}

drogon::Task<>
BuildController::finalize(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback,
                          std::string buildId) {
    const auto body = app::requireJsonObject(request);

    services::FinalizeBuildCommand command;
    command.files = readManifestFiles(body);
    command.entrypointRelativePath = app::requireString(body, "entrypoint");
    command.defaultLaunchArgs = app::optionalString(body, "launchArgs");

    auto finalized =
        co_await uploads().finalizeBuild(actorOf(request), std::move(buildId), std::move(command));
    if (!finalized.ok()) {
        fail(finalized.error());
    }

    callback(jsonResponse(request, buildToJson(finalized.value())));
    co_return;
}

drogon::Task<>
BuildController::manifest(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback,
                          std::string buildId) {
    auto document = co_await uploads().manifest(actorOf(request), std::move(buildId));
    if (!document.ok()) {
        fail(document.error());
    }

    // Served as the exact bytes the hash covers, not re-serialised through the JSON writer: a
    // client verifies its download by hashing this response and comparing with the header.
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k200OK);
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->setBody(document.value().json);
    response->addHeader("X-Manifest-Sha256", document.value().sha256);
    if (const auto requestId = app::requestIdOf(request); !requestId.empty()) {
        response->addHeader("X-Request-Id", requestId);
    }

    callback(response);
    co_return;
}

} // namespace launcher::controllers::v1
