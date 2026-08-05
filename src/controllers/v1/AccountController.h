#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

#include <functional>

namespace launcher::controllers::v1 {

/// What an account can do to itself. The first route on this surface, and for now the only one.
///
/// A POST rather than `DELETE /api/v1/me`, for one reason: the request carries a password, so
/// it needs a body, and a body on DELETE is the one place HTTP declines to promise anything —
/// no defined semantics, and intermediaries are free to drop it. The path also leaves room for
/// the deferred variant of this flow (read the pending request, cancel it) to be added without
/// re-cutting the surface, should a later session decide the grace period is worth its cost.
class AccountController : public drogon::HttpController<AccountController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AccountController::requestErasure,
                  "/api/v1/me/deletion",
                  drogon::Post,
                  "launcher::filters::JwtAuthFilter");
    METHOD_LIST_END

    drogon::Task<> requestErasure(drogon::HttpRequestPtr request,
                                  std::function<void(const drogon::HttpResponsePtr&)> callback);
};

} // namespace launcher::controllers::v1
