#pragma once

#include <string_view>

#include "common/Result.h"
#include "domain/LauncherRelease.h"

namespace launcher::app {

/// Reads a release document written by whoever signs releases, and refuses anything that is not
/// *byte for byte* the canonical form of what it parsed.
///
/// The round-trip check is the load-bearing part and is one line: parse, re-serialise, compare
/// with the input. Without it the row would store bytes whose meaning had only partly been
/// captured — an extra key, a different key order, a space after a colon — and the columns
/// derived from the parse would describe a document subtly unlike the one the signature covers.
/// With it, `document` and the columns beside it cannot mean two different things, and the
/// operator finds out at publish time instead of never.
///
/// It also means the signing tool's output format is pinned by this server: a document that
/// verifies here is one this server would have produced itself, which is what lets the client
/// treat the bytes as a wire contract rather than as arbitrary JSON.
///
/// This lives in `app/` rather than `domain/` because it needs a JSON parser and the domain
/// layer deliberately depends on nothing.
common::Result<domain::ReleaseDocument> parseReleaseDocument(std::string_view json);

} // namespace launcher::app
