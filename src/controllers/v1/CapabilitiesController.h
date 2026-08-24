#pragma once

#include <drogon/HttpController.h>

namespace launcher::controllers::v1 {

/// What this deployment will accept.
///
/// Unauthenticated on purpose, and it is the only route besides the health probes that is. A
/// launcher reads it at startup — before anybody has signed in — and everything in it is a
/// limit that has to be known to build a valid request at all. Nothing in the document depends
/// on who is asking.
///
/// Deliberately *not* folded into `/health`: a liveness probe is polled by an orchestrator on
/// a short timer and answers a question about the process, not about the contract.
class CapabilitiesController : public drogon::HttpController<CapabilitiesController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(CapabilitiesController::capabilities, "/api/v1/capabilities", drogon::Get);
    METHOD_LIST_END

    void capabilities(const drogon::HttpRequestPtr& request,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace launcher::controllers::v1
