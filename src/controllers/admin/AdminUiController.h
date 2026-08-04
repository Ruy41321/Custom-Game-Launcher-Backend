#pragma once

#include <drogon/HttpController.h>

#include <functional>

namespace launcher::controllers::admin {

/// Serves the operator console itself.
///
/// Only the listener gate applies: the page has to load before anybody can sign in, so it
/// carries no authentication filter. It contains no data — every number on it arrives from an
/// endpoint that does check — so what an unauthenticated caller on the loopback listener gets
/// is a login form.
class AdminUiController : public drogon::HttpController<AdminUiController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminUiController::index,
                  "/admin",
                  drogon::Get,
                  "launcher::filters::AdminSurfaceFilter");
    ADD_METHOD_TO(AdminUiController::index,
                  "/admin/",
                  drogon::Get,
                  "launcher::filters::AdminSurfaceFilter");
    METHOD_LIST_END

    void index(const drogon::HttpRequestPtr& request,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace launcher::controllers::admin
