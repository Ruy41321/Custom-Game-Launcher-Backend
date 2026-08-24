#pragma once

namespace launcher::auth {

/// The two pages the links in a message land on, embedded in the binary at build time from
/// `src/auth/ui/`.
///
/// They exist because a link in an email is opened by a browser, and every authentication
/// route here is a JSON `POST`: without a page, a verification link would be a URL nobody can
/// follow. Embedded rather than served from a directory for the same three reasons the admin
/// console is — no path to mount, nothing to fall out of step with the API, and integration
/// tests that exercise the bytes a deployment serves.
///
/// Neither page changes anything by being opened. A mail provider's link scanner fetches every
/// URL in a message, so a page that confirmed on load would consume the token before its owner
/// ever clicked and leave them looking at "this link is invalid".
extern const char* const VERIFY_EMAIL_HTML;

extern const char* const PASSWORD_RESET_HTML;

} // namespace launcher::auth
