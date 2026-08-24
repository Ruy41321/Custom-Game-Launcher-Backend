#pragma once

namespace launcher::domain::rules {

/// The stable names carried by `Error::rule` for the input-validation refusals, gathered here
/// so the whole contract can be read in one place and so production code and tests name the
/// same symbol rather than repeating a literal.
///
/// Three properties make this a contract rather than a convenience.
///
/// **They are frozen.** `detail` is prose for whoever reads the logs and is free to be
/// reworded; these are not. Renaming one is a breaking change to every client that translates
/// it, and the way to change a message is to change the message.
///
/// **There is one per corrective action, not one per branch.** Six different ways of writing a
/// malformed address are one `email_invalid`, because the person typing has one thing to do
/// about all six. A limit that was exceeded is its own rule and carries the limit in
/// `Error::ruleArgs`, because "at most 200 characters" is advice and "not accepted" is not.
///
/// **They cover the fields a person types into a form**, and deliberately nothing else. The
/// manifest paths, blob hashes, upload offsets, crash reports and release documents are values
/// a client computes, not text somebody typed: their refusals stay unnamed, their `detail`
/// already reads as the developer-facing message it is, and a client that showed a translated
/// sentence for one would be dressing up its own bug as the user's mistake.
///
/// A client that meets a rule it does not know falls back to the category, so adding one here
/// is additive and needs no coordinated release.

// Account fields — the sign-in, registration and settings screens.
inline constexpr const char* EMAIL_REQUIRED = "email_required";
inline constexpr const char* EMAIL_TOO_LONG = "email_too_long";
inline constexpr const char* EMAIL_INVALID = "email_invalid";
inline constexpr const char* PASSWORD_REQUIRED = "password_required";
inline constexpr const char* PASSWORD_TOO_SHORT = "password_too_short";
inline constexpr const char* PASSWORD_TOO_LONG = "password_too_long";
inline constexpr const char* PASSWORD_BLANK = "password_blank";
/// Only ever raised by the forced password change: re-entering the operator's one-time
/// password would clear the flag and leave the account on a credential somebody else knows.
inline constexpr const char* PASSWORD_UNCHANGED = "password_unchanged";
inline constexpr const char* DISPLAY_NAME_REQUIRED = "display_name_required";
inline constexpr const char* DISPLAY_NAME_TOO_SHORT = "display_name_too_short";
inline constexpr const char* DISPLAY_NAME_TOO_LONG = "display_name_too_long";
inline constexpr const char* DISPLAY_NAME_INVALID = "display_name_invalid";
inline constexpr const char* ERASURE_REASON_TOO_LONG = "erasure_reason_too_long";

// A game's own details.
inline constexpr const char* TITLE_REQUIRED = "title_required";
inline constexpr const char* TITLE_TOO_LONG = "title_too_long";
inline constexpr const char* SUMMARY_TOO_LONG = "summary_too_long";
inline constexpr const char* DESCRIPTION_TOO_LONG = "description_too_long";
inline constexpr const char* SLUG_REQUIRED = "slug_required";
inline constexpr const char* SLUG_TOO_LONG = "slug_too_long";
inline constexpr const char* SLUG_INVALID = "slug_invalid";
inline constexpr const char* RELEASE_DATE_INVALID = "release_date_invalid";

// Versions.
inline constexpr const char* VERSION_REQUIRED = "version_required";
inline constexpr const char* VERSION_INVALID = "version_invalid";
inline constexpr const char* VERSION_TOO_LARGE = "version_too_large";
inline constexpr const char* RELEASE_NOTES_TOO_LONG = "release_notes_too_long";
inline constexpr const char* BUILD_NAME_TOO_LONG = "build_name_too_long";

// Devlog entries. Named for the server's own `patch_notes`, which is what the wire contract
// calls them; a client is free to go on calling the feature a devlog.
inline constexpr const char* PATCH_NOTE_TITLE_REQUIRED = "patch_note_title_required";
inline constexpr const char* PATCH_NOTE_TITLE_TOO_LONG = "patch_note_title_too_long";
inline constexpr const char* PATCH_NOTE_BODY_TOO_LONG = "patch_note_body_too_long";

// Artwork.
inline constexpr const char* ALT_TEXT_TOO_LONG = "alt_text_too_long";
inline constexpr const char* ALT_TEXT_INVALID = "alt_text_invalid";

} // namespace launcher::domain::rules
