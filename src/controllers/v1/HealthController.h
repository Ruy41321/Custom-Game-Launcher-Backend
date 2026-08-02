#pragma once

#include <drogon/HttpController.h>

namespace launcher::controllers::v1 {

/// Liveness and readiness probes.
///
/// `/health` answers "is the process up" and must never touch a dependency — an orchestrator
/// restarting the container because the database blinked is worse than the outage itself.
/// `/health/ready` answers "can this instance serve traffic" and does check the database.
class HealthController : public drogon::HttpController<HealthController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HealthController::liveness, "/api/v1/health", drogon::Get);
    ADD_METHOD_TO(HealthController::readiness, "/api/v1/health/ready", drogon::Get);
    METHOD_LIST_END

    void liveness(const drogon::HttpRequestPtr& request,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void readiness(const drogon::HttpRequestPtr& request,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace launcher::controllers::v1
