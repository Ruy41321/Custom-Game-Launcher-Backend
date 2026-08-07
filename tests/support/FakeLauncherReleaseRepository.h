#pragma once

#include <drogon/utils/coroutine.h>

#include <optional>
#include <vector>

#include "repositories/ILauncherReleaseRepository.h"

namespace launcher::testing {

/// In-memory double for the release store.
///
/// It applies the same three rules the statement does — match the triple, skip retired rows,
/// newest version wins — because those are the behaviour the service depends on and a fake that
/// simply returned the first row would let a bug through unnoticed.
class FakeLauncherReleaseRepository : public repositories::ILauncherReleaseRepository {
  public:
    mutable std::vector<domain::LauncherRelease> releases;

    drogon::Task<std::optional<domain::LauncherRelease>>
    findLatest(domain::ReleaseQuery query) const override {
        std::optional<domain::LauncherRelease> best;
        for (const auto& release : releases) {
            if (release.parsed.channel != query.channel ||
                release.parsed.platform != query.platform || release.parsed.arch != query.arch ||
                !release.retiredAt.empty()) {
                continue;
            }
            if (!best.has_value() ||
                domain::isNewerRelease(release.parsed.version, best->parsed.version)) {
                best = release;
            }
        }
        co_return best;
    }
};

} // namespace launcher::testing
