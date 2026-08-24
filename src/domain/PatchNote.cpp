#include "domain/PatchNote.h"

#include "domain/Validation.h"
#include "domain/ValidationRules.h"

namespace launcher::domain {

using common::invalidInput;
using common::VoidResult;

VoidResult validatePatchNoteTitle(std::string_view title) {
    const auto trimmed = trim(title);
    if (trimmed.empty()) {
        return VoidResult::failure(
            invalidInput("title must not be empty", rules::PATCH_NOTE_TITLE_REQUIRED));
    }
    // Mirrors the patch_notes_title_length CHECK, so the refusal names the field instead of
    // arriving as a driver error.
    if (trimmed.size() > MAX_PATCH_NOTE_TITLE_LENGTH) {
        return VoidResult::failure(invalidInput(
            "title must be at most " + std::to_string(MAX_PATCH_NOTE_TITLE_LENGTH) + " characters",
            rules::PATCH_NOTE_TITLE_TOO_LONG,
            {std::to_string(MAX_PATCH_NOTE_TITLE_LENGTH)}));
    }
    return VoidResult::success();
}

VoidResult validatePatchNoteBody(std::string_view body) {
    if (body.size() > MAX_PATCH_NOTE_BODY_LENGTH) {
        return VoidResult::failure(invalidInput("bodyMarkdown must be at most " +
                                                    std::to_string(MAX_PATCH_NOTE_BODY_LENGTH) +
                                                    " characters",
                                                rules::PATCH_NOTE_BODY_TOO_LONG,
                                                {std::to_string(MAX_PATCH_NOTE_BODY_LENGTH)}));
    }
    return VoidResult::success();
}

} // namespace launcher::domain
