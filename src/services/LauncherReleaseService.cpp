#include "services/LauncherReleaseService.h"

#include <spdlog/spdlog.h>

#include <utility>

#include "common/Logging.h"
#include "common/Signature.h"

namespace launcher::services {
namespace {

using common::ErrorCode;
using common::Result;

std::string withoutTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

} // namespace

LauncherReleaseService::LauncherReleaseService(
    const repositories::ILauncherReleaseRepository& releases, LauncherReleaseSettings settings)
    : releases_(releases),
      settings_(std::move(settings)) {}

bool LauncherReleaseService::enabled() const {
    return !settings_.publicKey.empty();
}

drogon::Task<Result<LatestRelease>>
LauncherReleaseService::latest(domain::ReleaseQuery query) const {
    if (!enabled()) {
        co_return Result<LatestRelease>::failure(ErrorCode::NotFound,
                                                 "this server publishes no launcher releases");
    }

    const auto found = co_await releases_.findLatest(query);
    if (!found.has_value()) {
        co_return Result<LatestRelease>::failure(ErrorCode::NotFound,
                                                 "this server publishes no launcher releases");
    }

    // Checked again on the way out, although `--publish-release` refused to store a row whose
    // signature did not verify. It costs one verification on a route a launcher hits once per
    // start, and it buys two things the publish-time check cannot. A row altered in the database
    // afterwards stops being served here rather than being rejected on every client, which is
    // the difference between one log line naming the release and a fleet quietly failing to
    // update. And a key rotated in the configuration while older rows were signed with the
    // previous one is otherwise invisible: every launcher would refuse every document, and
    // nothing on this side would say why.
    if (const auto verified =
            common::verifyP256Signature(settings_.publicKey, found->signature, found->document);
        !verified.ok()) {
        spdlog::error("refusing to serve launcher release {}: {}",
                      common::escapeJson(found->id),
                      common::escapeJson(verified.error().detail));
        co_return Result<LatestRelease>::failure(ErrorCode::NotFound,
                                                 "this server publishes no launcher releases");
    }

    LatestRelease latest;
    latest.document = found->document;
    latest.signature = found->signature;
    latest.url = withoutTrailingSlash(settings_.publicBaseUrl) + "/" + found->storageKey;
    co_return Result<LatestRelease>::success(std::move(latest));
}

} // namespace launcher::services
