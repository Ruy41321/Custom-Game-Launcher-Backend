#pragma once

#include <string>
#include <string_view>

namespace launcher::domain {

/// JSON string escaping for the canonical documents this server signs and hashes.
///
/// Written here rather than delegated to jsoncpp because a canonical form is a wire contract:
/// `builds.manifest_sha256` and every launcher-release signature cover exactly these bytes, so
/// the encoding must not shift if the JSON library ever changes how it emits non-ASCII or
/// solidus characters. It lives in `domain/` for the same reason `mayViewGame` does — two
/// documents now depend on it agreeing with itself, and a rule with two copies stops being a
/// rule the first time one of them is edited.
///
/// Only the escapes JSON requires are emitted: quote, backslash, the five short forms, and
/// `\u00xx` for the remaining control characters. Everything else, UTF-8 included, is passed
/// through unchanged.
void appendCanonicalJsonString(std::string& out, std::string_view value);

} // namespace launcher::domain
