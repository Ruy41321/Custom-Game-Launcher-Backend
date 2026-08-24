#pragma once

#include <drogon/utils/coroutine.h>

#include <string>

#include "common/Result.h"
#include "domain/LauncherRelease.h"
#include "repositories/ILauncherReleaseRepository.h"

namespace launcher::services {

struct LauncherReleaseSettings {
    /// base64 DER SubjectPublicKeyInfo for the P-256 key releases are signed with.
    ///
    /// **Empty turns the whole surface off**, and that is the default. A deployment that has not
    /// set up signing serves no releases at all rather than serving unverifiable ones — the same
    /// answer the client gives when its own embedded key is absent, arrived at from the other
    /// end. There is deliberately no way to serve a release without a signature: an update
    /// mechanism with the checking switched off is worse than no update mechanism, because it is
    /// a channel straight into every installation that trusts it.
    std::string publicKey;

    /// Where the file server answers, path included, e.g. `http://localhost:8081/launcher`.
    std::string publicBaseUrl;
};

/// What a launcher gets back when it asks about the newest release.
struct LatestRelease {
    /// The exact bytes the signature covers. Served verbatim; the client verifies these.
    std::string document;
    std::string signature;
    /// Where the artifact is. Derived here rather than left to the client, which would
    /// otherwise have to be told the file server's address separately — and it is not a thing
    /// the client has to trust, because the bytes it fetches are checked against the content
    /// address inside the signed document.
    std::string url;
};

/// Serving launcher releases, and nothing else.
///
/// This service cannot create one. Publishing happens from the command line, against a document
/// signed on a machine that is not this one, and the private key exists nowhere in this
/// deployment — so an attacker holding this server, this database and this disk can stop
/// launchers from updating but cannot make them update to something. That property is the
/// reason this feature is built the way it is, and it is the only property in this repository
/// that survives a full compromise.
class LauncherReleaseService {
  public:
    LauncherReleaseService(const repositories::ILauncherReleaseRepository& releases,
                           LauncherReleaseSettings settings);

    /// False when no signing key is configured. Published in the capabilities document so a
    /// launcher can stop asking rather than treating every start-up as a failed check.
    bool enabled() const;

    /// The newest live release for the triple, or `NotFound`.
    ///
    /// `NotFound` covers three different situations on purpose — no key configured, no release
    /// published, none published for this platform — because from a client's side they are one
    /// situation: there is nothing to update to. Distinguishing them would tell a stranger which
    /// platforms a deployment builds for, and would give a client three states where it can act
    /// on one.
    drogon::Task<common::Result<LatestRelease>> latest(domain::ReleaseQuery query) const;

  private:
    const repositories::ILauncherReleaseRepository& releases_;
    LauncherReleaseSettings settings_;
};

} // namespace launcher::services
