#include "controllers/v1/LauncherController.h"

#include <json/json.h>

#include <string>

#include "app/AppContext.h"
#include "app/HttpError.h"
#include "controllers/v1/CatalogJson.h"
#include "services/LauncherReleaseService.h"

namespace launcher::controllers::v1 {
namespace {

common::Result<domain::ReleaseQuery> queryFrom(const drogon::HttpRequestPtr& request) {
    domain::ReleaseQuery query;

    // Channel defaults, platform and architecture do not. The default is the conservative one:
    // a caller that says nothing gets the stream everybody is on, never a pre-release. There is
    // no equally safe default for the other two — guessing which build of the launcher somebody
    // is about to install is guessing at the program that will replace theirs.
    const auto channel = request->getParameter("channel");
    if (!channel.empty()) {
        const auto parsed = domain::parseReleaseChannel(channel);
        if (!parsed.has_value()) {
            // A typo is refused rather than read as `stable`: silently moving a caller onto a
            // different stream than the one they named is the failure this is here to prevent.
            return common::Result<domain::ReleaseQuery>::failure(
                common::ErrorCode::InvalidInput, "channel must be 'stable' or 'beta'");
        }
        query.channel = *parsed;
    }

    const auto platform = domain::parseReleasePlatform(request->getParameter("platform"));
    if (!platform.has_value()) {
        return common::Result<domain::ReleaseQuery>::failure(
            common::ErrorCode::InvalidInput,
            "platform is required and must be 'windows', 'linux' or 'macos'");
    }
    query.platform = *platform;

    const auto arch = domain::parseReleaseArch(request->getParameter("arch"));
    if (!arch.has_value()) {
        return common::Result<domain::ReleaseQuery>::failure(
            common::ErrorCode::InvalidInput, "arch is required and must be 'x64' or 'arm64'");
    }
    query.arch = *arch;

    return common::Result<domain::ReleaseQuery>::success(query);
}

} // namespace

drogon::Task<>
LauncherController::latest(drogon::HttpRequestPtr request,
                           std::function<void(const drogon::HttpResponsePtr&)> callback) {
    const auto query = queryFrom(request);
    if (!query.ok()) {
        throw common::ApiException(query.error());
    }

    const auto found =
        co_await app::AppContext::instance().launcherReleaseService().latest(query.value());
    if (!found.ok()) {
        throw common::ApiException(found.error());
    }

    // The document travels as an opaque string, not as a nested object, and that is the whole
    // contract. The signature covers those exact bytes, so re-serialising them into JSON here
    // would hand the client something it could not check — and asking the client to rebuild a
    // canonical form would put a second definition of this document in a second language. It is
    // the same rule `GET /builds/{id}/manifest` follows: serve the bytes the hash covers.
    Json::Value response;
    response["document"] = found.value().document;
    response["signature"] = found.value().signature;
    response["url"] = found.value().url;

    auto httpResponse = jsonResponse(request, response);
    // A release changes a few times a year and a launcher asks once per start, so this is about
    // a client opening twice in a minute rather than about load.
    httpResponse->addHeader("Cache-Control", "public, max-age=60");
    callback(httpResponse);
    co_return;
}

} // namespace launcher::controllers::v1
