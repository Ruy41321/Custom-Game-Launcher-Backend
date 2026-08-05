#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

#include <functional>
#include <string>

namespace launcher::controllers::admin {

/// Reading what launchers have sent.
///
/// Three routes rather than one, because an operator asks two different questions and they
/// want different answers: *what is going wrong* is a list of distinct bugs, and *what happened
/// to this one* is the reports behind it. A single list of every report answers the second
/// question badly and the first not at all — a thousand rows of the same crash.
///
/// Every one needs `admin.crashes.read`. A stack trace is a map of the program, and a list of
/// them is a list of ways to break it.
class AdminCrashController : public drogon::HttpController<AdminCrashController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminCrashController::groups,
                  "/admin/api/crashes",
                  drogon::Get,
                  "launcher::filters::AdminSurfaceFilter",
                  "launcher::filters::JwtAuthFilter",
                  "launcher::filters::AdminOperatorFilter");
    ADD_METHOD_TO(AdminCrashController::reports,
                  "/admin/api/crashes/reports",
                  drogon::Get,
                  "launcher::filters::AdminSurfaceFilter",
                  "launcher::filters::JwtAuthFilter",
                  "launcher::filters::AdminOperatorFilter");
    ADD_METHOD_TO(AdminCrashController::report,
                  "/admin/api/crashes/reports/{1}",
                  drogon::Get,
                  "launcher::filters::AdminSurfaceFilter",
                  "launcher::filters::JwtAuthFilter",
                  "launcher::filters::AdminOperatorFilter");
    METHOD_LIST_END

    /// The distinct bugs, most recently seen first.
    drogon::Task<> groups(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback);

    /// The individual reports, optionally narrowed to one `fingerprint`.
    drogon::Task<> reports(drogon::HttpRequestPtr request,
                           std::function<void(const drogon::HttpResponsePtr&)> callback);

    drogon::Task<> report(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback,
                          std::string reportId);
};

} // namespace launcher::controllers::admin
