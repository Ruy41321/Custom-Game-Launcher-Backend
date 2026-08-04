#pragma once

namespace launcher::admin {

/// The operator console, embedded in the binary at build time from
/// `src/admin/ui/index.html`.
///
/// Embedded rather than served from a directory so the deployed image has no path to mount and
/// nothing to get out of step with the API it talks to, and so the integration tests exercise
/// the same bytes a deployment serves. The source stays a real .html file; CMake turns it into
/// a string literal.
extern const char* const INDEX_HTML;

} // namespace launcher::admin
