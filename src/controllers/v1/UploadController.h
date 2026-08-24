#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

#include <functional>
#include <string>

namespace launcher::controllers::v1 {

/// One resumable upload.
///
/// The protocol is deliberately the same shape as tus: `GET` reports how many bytes the
/// server has, `PATCH` sends the next chunk with an `Upload-Offset` header, `DELETE` gives up.
/// A client that lost its place asks, it does not guess — and a `PATCH` at the wrong offset is
/// refused with the real one rather than corrupting the file.
class UploadController : public drogon::HttpController<UploadController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(UploadController::status,
                  "/api/v1/uploads/{1}",
                  drogon::Get,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(UploadController::uploadChunk,
                  "/api/v1/uploads/{1}",
                  drogon::Patch,
                  "launcher::filters::JwtAuthFilter");
    ADD_METHOD_TO(UploadController::abort,
                  "/api/v1/uploads/{1}",
                  drogon::Delete,
                  "launcher::filters::JwtAuthFilter");
    METHOD_LIST_END

    drogon::Task<> status(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback,
                          std::string sessionId);

    drogon::Task<> uploadChunk(drogon::HttpRequestPtr request,
                               std::function<void(const drogon::HttpResponsePtr&)> callback,
                               std::string sessionId);

    drogon::Task<> abort(drogon::HttpRequestPtr request,
                         std::function<void(const drogon::HttpResponsePtr&)> callback,
                         std::string sessionId);
};

} // namespace launcher::controllers::v1
