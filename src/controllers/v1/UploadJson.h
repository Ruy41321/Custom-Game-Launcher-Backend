#pragma once

#include <json/json.h>

#include "repositories/IUploadSessionRepository.h"

namespace launcher::controllers::v1 {

/// An upload session as the client sees it. The offset also travels as an `Upload-Offset`
/// header, so a client can act on it without parsing the body.
Json::Value uploadSessionToJson(const repositories::UploadSession& session);

} // namespace launcher::controllers::v1
