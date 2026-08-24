#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace launcher::storage {

struct SignedUrlSettings {
    /// Where the file server answers, including the path it serves blobs under — the same
    /// prefix nginx sees as `$uri`, because that is what the signature covers.
    std::string publicBaseUrl{"http://localhost:8081/files"};
    /// Shared with the file server as FILE_SECURE_LINK_SECRET. Nothing else may derive a URL.
    std::string secret;
    /// How long a minted URL stays usable. Long enough for a large file on a slow line, short
    /// enough that a leaked URL stops working.
    std::chrono::seconds ttl{3600};
};

/// Mints the download URLs nginx's `secure_link` module validates.
///
/// The API never serves blob bytes: it signs a URL, and nginx checks the signature with no
/// callback per request and handles `Range` natively, so a multi-gigabyte transfer never
/// occupies an API worker. See decision D7 in CLAUDE.md.
///
/// The signature is exactly the format the module expects:
///
///     token = base64url( md5( "<expires><uri> <secret>" ) )
///
/// MD5 is not a choice made here — it is the module's wire format. The construction puts the
/// secret *last*, so the length-extension weakness of a prefix-keyed MD5 does not apply, and
/// forging a URL means recovering the secret rather than extending a digest.
class DownloadUrlSigner {
  public:
    explicit DownloadUrlSigner(SignedUrlSettings settings);

    /// The absolute expiry a URL minted at `now` carries, as seconds since the epoch. Taken as
    /// a parameter so a plan can stamp every one of its URLs with the same instant, and so
    /// tests are not at the mercy of the clock.
    int64_t expiryFor(std::chrono::system_clock::time_point now) const;

    /// A signed URL for the blob stored under `storageKey` (`ab/cd/<sha256>`).
    std::string sign(std::string_view storageKey, int64_t expiresAtUnixSeconds) const;

    const SignedUrlSettings& settings() const { return settings_; }

  private:
    SignedUrlSettings settings_;
    /// The path component of publicBaseUrl, with no trailing slash: what nginx compares
    /// against as `$uri`, and therefore what has to go into the signature rather than the
    /// scheme and host the client happens to reach the file server through.
    std::string basePath_;
    std::string baseUrl_;
};

} // namespace launcher::storage
