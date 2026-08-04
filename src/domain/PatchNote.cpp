#include "domain/PatchNote.h"

#include "domain/Validation.h"

namespace launcher::domain {

using common::ErrorCode;
using common::VoidResult;

VoidResult validatePatchNoteTitle(std::string_view title) {
    const auto trimmed = trim(title);
    if (trimmed.empty()) {
        return VoidResult::failure(ErrorCode::InvalidInput, "title must not be empty");
    }
    // Mirrors the patch_notes_title_length CHECK, so the refusal names the field instead of
    // arriving as a driver error.
    if (trimmed.size() > MAX_PATCH_NOTE_TITLE_LENGTH) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "title must be at most " +
                                       std::to_string(MAX_PATCH_NOTE_TITLE_LENGTH) + " characters");
    }
    return VoidResult::success();
}

VoidResult validatePatchNoteBody(std::string_view body) {
    if (body.size() > MAX_PATCH_NOTE_BODY_LENGTH) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "bodyMarkdown must be at most " +
                                       std::to_string(MAX_PATCH_NOTE_BODY_LENGTH) + " characters");
    }
    return VoidResult::success();
}

} // namespace launcher::domain
