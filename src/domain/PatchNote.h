#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "common/Result.h"

namespace launcher::domain {

inline constexpr std::size_t MAX_PATCH_NOTE_TITLE_LENGTH = 200;
inline constexpr std::size_t MAX_PATCH_NOTE_BODY_LENGTH = 40000;

common::VoidResult validatePatchNoteTitle(std::string_view title);

common::VoidResult validatePatchNoteBody(std::string_view body);

/// An entry in a game's devlog.
///
/// Deliberately separate from `GameVersion::releaseNotes`, which describes exactly one
/// version and is written by whoever published it. A patch note may name a version or none at
/// all — "what we are working on this month" is a legitimate post — and it has a publication
/// state of its own, so a draft can be written before the build it talks about exists.
struct PatchNote {
    std::string id;
    std::string gameId;
    /// Empty when the note is not about one particular version.
    std::string gameVersionId;
    std::string title;
    std::string bodyMarkdown;
    std::string authorUserId;
    std::string authorDisplayName;
    /// Empty while the note is still a draft.
    std::string publishedAt;
    std::string createdAt;
    std::string updatedAt;

    bool published() const { return !publishedAt.empty(); }
};

struct NewPatchNote {
    std::string gameId;
    std::string gameVersionId;
    std::string title;
    std::string bodyMarkdown;
    std::string authorUserId;
    bool publish{false};
};

/// A partial update. An absent field is left untouched.
struct PatchNoteUpdate {
    std::optional<std::string> title;
    std::optional<std::string> bodyMarkdown;
    /// An empty string detaches the note from its version.
    std::optional<std::string> gameVersionId;
    /// Publishing and unpublishing are the same field, because a note that went out by
    /// mistake has to be able to come back.
    std::optional<bool> published;

    bool empty() const { return !title && !bodyMarkdown && !gameVersionId && !published; }
};

} // namespace launcher::domain
