#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/Result.h"
#include "domain/PatchNote.h"

namespace launcher::repositories {

inline constexpr int DEFAULT_PATCH_NOTE_PAGE_SIZE = 20;
inline constexpr int MAX_PATCH_NOTE_PAGE_SIZE = 100;

struct PatchNoteQuery {
    std::string gameId;
    /// The publisher's own view includes drafts; everybody else only ever sees published
    /// notes, whatever the request said.
    bool includeUnpublished{false};
    int limit{DEFAULT_PATCH_NOTE_PAGE_SIZE};
    int offset{0};
};

struct PatchNotePage {
    std::vector<domain::PatchNote> items;
    int64_t total{0};
};

/// Persistence for a game's devlog. Every parameter is taken by value — a coroutine's
/// reference parameters dangle as soon as it first suspends.
class IPatchNoteRepository {
  public:
    virtual ~IPatchNoteRepository() = default;

    virtual drogon::Task<common::Result<domain::PatchNote>>
    create(domain::NewPatchNote note) const = 0;

    virtual drogon::Task<std::optional<domain::PatchNote>> findById(std::string id) const = 0;

    virtual drogon::Task<PatchNotePage> search(PatchNoteQuery query) const = 0;

    virtual drogon::Task<std::optional<domain::PatchNote>>
    update(std::string id, domain::PatchNoteUpdate changes) const = 0;

    virtual drogon::Task<bool> remove(std::string id) const = 0;
};

} // namespace launcher::repositories
