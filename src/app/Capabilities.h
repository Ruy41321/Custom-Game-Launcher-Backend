#pragma once

#include <json/json.h>

#include "app/Config.h"

namespace launcher::app {

/// The limits a client has to know before it can talk to this deployment successfully.
///
/// Every number in here was previously a constant compiled into the launcher, guessed from the
/// defaults in this repository. A deployment that lowered `uploads.maxChunkBytes` therefore
/// broke every upload with an error that did not say what the real limit was, and a deployment
/// that raised it got 4 MiB chunks for ever. The document exists so that stops being true.
///
/// A pure function of the configuration, so it can be asserted on without an HTTP server, and
/// so the endpoint has nothing in it but serialisation.
///
/// It carries nothing about the caller: `defaultQuotaBytes` is what a *new* account is given,
/// not what anybody has left. That is what makes the route safe to serve unauthenticated, and
/// unauthenticated is what makes it readable at startup, before a session exists.
Json::Value capabilitiesDocument(const AppConfig& config);

} // namespace launcher::app
