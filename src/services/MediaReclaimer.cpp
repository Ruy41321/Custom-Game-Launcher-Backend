#include "services/MediaReclaimer.h"

#include <utility>

namespace launcher::services {

MediaReclaimer::MediaReclaimer(const repositories::IMediaRepository& media,
                               storage::MediaStore store)
    : media_(media),
      store_(std::move(store)) {}

drogon::Task<> MediaReclaimer::reclaim(std::string storageKey) const {
    if (storageKey.empty()) {
        co_return;
    }

    // The row is already gone by the time this runs, so this asks whether any *other* row still
    // holds the key. Asking first is what keeps a delete on one game from blanking another's.
    const bool stillUsed = co_await media_.isStorageKeyReferenced(storageKey);
    if (!stillUsed) {
        store_.remove(storageKey);
    }
    co_return;
}

drogon::Task<> MediaReclaimer::reclaimAll(std::vector<std::string> storageKeys) const {
    for (auto& storageKey : storageKeys) {
        co_await reclaim(std::move(storageKey));
    }
    co_return;
}

} // namespace launcher::services
