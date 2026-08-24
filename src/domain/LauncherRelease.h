#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "common/Result.h"
#include "domain/Semver.h"

namespace launcher::domain {

/// Which stream a launcher release belongs to.
///
/// Two, and which one a launcher is on is decided by whoever packaged it — `launcher.config.json`
/// is the fork-and-rebrand surface — rather than by a user setting. A player who could move
/// themselves onto a channel their distributor never published to would be a player who can
/// break their own installation with no way back, and the launcher is the program that has to
/// still start in order to fix anything.
enum class ReleaseChannel {
    Stable,
    Beta,
};

/// The three operating systems the client is published for.
enum class ReleasePlatform {
    Windows,
    Linux,
    MacOS,
};

enum class ReleaseArch {
    X64,
    Arm64,
};

std::optional<ReleaseChannel> parseReleaseChannel(std::string_view value);
std::optional<ReleasePlatform> parseReleasePlatform(std::string_view value);
std::optional<ReleaseArch> parseReleaseArch(std::string_view value);

const char* nameFor(ReleaseChannel channel);
const char* nameFor(ReleasePlatform platform);
const char* nameFor(ReleaseArch arch);

/// The only schema version this server knows how to produce or check. A document declaring
/// anything else is refused at publish time rather than stored for a client to puzzle over.
inline constexpr int RELEASE_DOCUMENT_SCHEMA = 1;

/// A self-contained launcher build is tens of megabytes; this is room for several times that
/// and still a bound, so a mistyped size cannot describe an artifact no disk will hold.
inline constexpr int64_t MAX_RELEASE_ARTIFACT_BYTES = 4LL * 1024 * 1024 * 1024;

/// Release notes are a paragraph shown in the launcher, not a changelog file.
inline constexpr std::size_t MAX_LAUNCHER_RELEASE_NOTES_LENGTH = 4096;

/// What a signature covers.
///
/// **The document is signed, not the artifact**, and the difference is the whole security
/// argument. A signature over the bytes of a zip says only that somebody with the key once
/// produced that zip; it says nothing about which version it is, which channel it belongs to or
/// which platform it runs on. An attacker holding this database could then serve a genuine,
/// genuinely signed artifact as something it is not — last year's build as the newest one, or
/// the Linux build to a Windows launcher. Binding all of it together in one signed document is
/// what makes that impossible, and it is the same reasoning that keeps the build id *out* of a
/// manifest: what a hash covers is a decision, not an accident.
struct ReleaseDocument {
    int schema{RELEASE_DOCUMENT_SCHEMA};
    ReleaseChannel channel{ReleaseChannel::Stable};
    Semver version;
    ReleasePlatform platform{ReleasePlatform::Windows};
    ReleaseArch arch{ReleaseArch::X64};
    /// Content address of the artifact. The client refuses bytes that do not hash to it, so a
    /// tampered download fails even when the document and its signature are untouched.
    std::string artifactSha256;
    int64_t artifactSize{0};
    /// `YYYY-MM-DDTHH:MM:SSZ`, the shape every other timestamp on this API takes.
    std::string releasedAt;
    std::string notes;
};

/// Rejects a document that could not have been meant.
///
/// Everything here arrives from an operator's own machine, so these are typo checks rather than
/// defences — but they run before a row is written, which is the difference between finding a
/// mistake once at publish time and finding it on every launcher that asks.
common::VoidResult validateReleaseDocument(const ReleaseDocument& document);

/// Byte-exact serialisation of a release document.
///
/// The signature covers exactly these bytes and the route serves exactly these bytes, so a
/// client verifies by checking the signature over what arrived — it never has to reproduce a
/// canonical form of its own. Fixed key order, no insignificant whitespace, and a hand-written
/// serialiser, for the reasons `canonicalManifestDocument` gives.
std::string canonicalReleaseDocument(const ReleaseDocument& document);

/// True when `candidate` is a strictly newer version than `installed`.
///
/// Ordering is numeric on all three components, because compared as text `0.10.0` sorts before
/// `0.9.0`. Strict is the point: this is what makes a correctly signed *old* document useless
/// to somebody replaying it, which is the one attack a valid signature cannot answer on its own.
bool isNewerRelease(const Semver& candidate, const Semver& installed);

/// A stored release: the document, the signature over it, and where the artifact sits.
struct LauncherRelease {
    std::string id;
    ReleaseDocument parsed;
    /// The exact bytes `signature` covers, kept verbatim rather than re-serialised on read.
    std::string document;
    /// base64 of the DER ECDSA signature.
    std::string signature;
    /// `ab/cd/<sha256>.zip` under the release root.
    std::string storageKey;
    std::string createdAt;
    /// Empty while the release is live.
    std::string retiredAt;
};

/// Which release a launcher is asking about. All three are required: a launcher knows its own
/// channel, platform and architecture, and a default for any of them would be a guess made on
/// behalf of somebody about to replace their own program.
struct ReleaseQuery {
    ReleaseChannel channel{ReleaseChannel::Stable};
    ReleasePlatform platform{ReleasePlatform::Windows};
    ReleaseArch arch{ReleaseArch::X64};
};

} // namespace launcher::domain
