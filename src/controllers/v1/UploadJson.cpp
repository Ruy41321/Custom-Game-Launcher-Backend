#include "controllers/v1/UploadJson.h"

namespace launcher::controllers::v1 {

Json::Value uploadSessionToJson(const repositories::UploadSession& session) {
    Json::Value json;
    json["id"] = session.id;
    json["buildId"] = session.buildId;
    json["sha256"] = session.blobSha256;
    json["sizeBytes"] = static_cast<Json::Int64>(session.declaredSizeBytes);
    json["receivedBytes"] = static_cast<Json::Int64>(session.receivedBytes);
    json["status"] = repositories::toDatabaseValue(session.status);
    json["complete"] = session.complete();
    return json;
}

} // namespace launcher::controllers::v1
