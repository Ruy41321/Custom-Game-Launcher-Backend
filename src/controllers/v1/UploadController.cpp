#include "controllers/v1/UploadController.h"

#include <json/json.h>

#include <charconv>
#include <string>
#include <utility>

#include "app/AppContext.h"
#include "app/HttpError.h"
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

/// The offset the client believes it is writing at. Required rather than defaulted: guessing
/// on the client's behalf is exactly how a resumed upload silently duplicates or skips a
/// range, and the hash check would only catch it after the whole file had been sent.
int64_t requireUploadOffset(const drogon::HttpRequestPtr& request) {
    const std::string& header = request->getHeader("Upload-Offset");
    if (header.empty()) {
        throw ApiException(ErrorCode::InvalidInput,
                           "an Upload-Offset header is required; GET this session to find the "
                           "offset to resume from");
    }

    int64_t offset = 0;
    const auto* const end = header.data() + header.size();
    const auto parsed = std::from_chars(header.data(), end, offset);
    if (parsed.ec != std::errc{} || parsed.ptr != end || offset < 0) {
        throw ApiException(ErrorCode::InvalidInput,
                           "Upload-Offset must be a non-negative whole number of bytes");
    }
    return offset;
}

drogon::HttpResponsePtr sessionResponse(const drogon::HttpRequestPtr& request,
                                        const repositories::UploadSession& session) {
    auto response = jsonResponse(request, uploadSessionToJson(session));
    response->addHeader("Upload-Offset", std::to_string(session.receivedBytes));
    return response;
}

} // namespace

drogon::Task<>
UploadController::status(drogon::HttpRequestPtr request,
                         std::function<void(const drogon::HttpResponsePtr&)> callback,
                         std::string sessionId) {
    auto session = co_await uploads().sessionStatus(actorOf(request), std::move(sessionId));
    if (!session.ok()) {
        fail(session.error());
    }

    callback(sessionResponse(request, session.value()));
    co_return;
}

drogon::Task<>
UploadController::uploadChunk(drogon::HttpRequestPtr request,
                              std::function<void(const drogon::HttpResponsePtr&)> callback,
                              std::string sessionId) {
    const auto offset = requireUploadOffset(request);

    auto session = co_await uploads().uploadChunk(
        actorOf(request), std::move(sessionId), offset, std::string(request->getBody()));
    if (!session.ok()) {
        fail(session.error());
    }

    callback(sessionResponse(request, session.value()));
    co_return;
}

drogon::Task<> UploadController::abort(drogon::HttpRequestPtr request,
                                       std::function<void(const drogon::HttpResponsePtr&)> callback,
                                       std::string sessionId) {
    const auto aborted = co_await uploads().abortUpload(actorOf(request), std::move(sessionId));
    if (!aborted.ok()) {
        fail(aborted.error());
    }

    Json::Value body;
    body["status"] = "aborted";
    callback(jsonResponse(request, body));
    co_return;
}

} // namespace launcher::controllers::v1
