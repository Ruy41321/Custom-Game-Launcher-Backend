#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

#include <functional>

namespace launcher::controllers::v1 {

/// Where a launcher sends what it wrote down when it died.
///
/// **Unauthenticated, and that is the decision.** A launcher crashes on the sign-in screen as
/// readily as anywhere else — more readily, since that is where a broken configuration shows —
/// and a route that only a signed-in client could reach would be missing exactly the failures
/// worth having. What stands in for a token is `CrashRateLimitFilter` and a body the server
/// caps field by field.
///
/// Nothing sent here names an account, and the route does not look for one even if a token
/// happens to be attached. See the head of migration 0004.
class CrashReportController : public drogon::HttpController<CrashReportController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(CrashReportController::submit,
                  "/api/v1/crash-reports",
                  drogon::Post,
                  "launcher::filters::CrashRateLimitFilter");
    METHOD_LIST_END

    drogon::Task<> submit(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback);
};

} // namespace launcher::controllers::v1
