#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

#include <functional>

namespace launcher::controllers::v1 {

/// Where a launcher asks whether there is a newer launcher.
///
/// **Unauthenticated, and for a sharper reason than the crash-report route.** That one is
/// unauthenticated because a crash on the sign-in screen is worth having; this one is
/// unauthenticated because the launcher that most needs an update is the one that *cannot* sign
/// in — pointed at a server it has never reached, holding an address nobody confirmed, or
/// carrying the bug that the update fixes. A route behind a token would be missing exactly the
/// installations an update mechanism exists for.
///
/// It is also why this surface is not part of the catalog. Every catalog route carries an
/// `Actor` and asks `mayViewGame`; reaching a release through it would mean an unauthenticated
/// path inside `CatalogService`, which is the one place this server has spent four milestones
/// concentrating its authorization rules.
///
/// Nothing here is secret and nothing depends on who is asking, so — like `/capabilities` — the
/// answer is briefly cacheable and carries no per-caller state at all.
class LauncherController : public drogon::HttpController<LauncherController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(LauncherController::latest, "/api/v1/launcher/releases/latest", drogon::Get);
    METHOD_LIST_END

    drogon::Task<> latest(drogon::HttpRequestPtr request,
                          std::function<void(const drogon::HttpResponsePtr&)> callback);
};

} // namespace launcher::controllers::v1
