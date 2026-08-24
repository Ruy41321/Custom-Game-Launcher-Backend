#pragma once

#include <json/json.h>

#include "services/DownloadService.h"

namespace launcher::controllers::v1 {

Json::Value downloadPlanToJson(const services::DownloadPlan& plan);

Json::Value integrityReportToJson(const services::IntegrityReport& report);

} // namespace launcher::controllers::v1
