#pragma once

#include <vector>

#include "repositories/IDownloadRepository.h"

namespace launcher::testing {

/// Records the download events a plan produced, so the tests can assert that what a client was
/// handed is what the publisher's statistics will show.
///
/// The interface method is const because callers only read through it, so the recorded state is
/// mutable.
class FakeDownloadRepository : public repositories::IDownloadRepository {
  public:
    mutable std::vector<repositories::DownloadEvent> events;

    drogon::Task<bool> recordEvent(repositories::DownloadEvent event) const override {
        events.push_back(std::move(event));
        co_return true;
    }
};

} // namespace launcher::testing
