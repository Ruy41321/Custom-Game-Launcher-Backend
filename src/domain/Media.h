#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "common/Result.h"

namespace launcher::domain {

/// Mirrors the game_media_kind enum created by migration 0001.
///
/// The first three are a game's identity and there is exactly one of each; screenshots are a
/// gallery and are ordered by sort_order. That difference is enforced by a partial unique
/// index rather than by a service, so no route can create a second cover.
enum class MediaKind { Cover, Banner, Logo, Screenshot };

const char* toString(MediaKind value);

std::optional<MediaKind> parseMediaKind(std::string_view value);

/// True for the kinds a game has at most one of.
bool isSingletonKind(MediaKind kind);

/// The image formats this server is willing to store and, more to the point, to serve.
///
/// SVG is deliberately absent: it is a document format that can carry script, and media is
/// served from a public location with no signature, so an uploaded SVG would be a stored
/// cross-site scripting vector rather than a picture.
enum class ImageFormat { Png, Jpeg, WebP };

const char* contentTypeOf(ImageFormat format);

/// The extension the storage key ends in. Present so nginx answers with a usable
/// `Content-Type` from its own mime table instead of `application/octet-stream`.
const char* extensionOf(ImageFormat format);

/// Identifies an image by its leading bytes.
///
/// The uploader's `Content-Type` header is not consulted at all. What a client claims about a
/// body it controls decides nothing here, precisely because the answer ends up as the
/// `Content-Type` of a public URL.
std::optional<ImageFormat> sniffImageFormat(std::string_view bytes);

inline constexpr std::size_t MAX_ALT_TEXT_LENGTH = 300;

/// A gallery, not an archive. The cap exists so one game cannot fill the media volume, and it
/// is checked before any byte is written.
inline constexpr int MAX_SCREENSHOTS_PER_GAME = 12;

common::VoidResult validateAltText(std::string_view altText);

struct GameMedia {
    std::string id;
    std::string gameId;
    MediaKind kind{MediaKind::Screenshot};
    /// Path under the media root, as served: `ab/cd/<sha256>.<ext>`.
    std::string storageKey;
    std::string sha256;
    std::string contentType;
    int64_t sizeBytes{0};
    std::string altText;
    int sortOrder{0};
    std::string createdAt;
};

struct NewGameMedia {
    std::string gameId;
    MediaKind kind{MediaKind::Screenshot};
    std::string storageKey;
    std::string sha256;
    std::string contentType;
    int64_t sizeBytes{0};
    std::string altText;
    int sortOrder{0};
};

/// A partial update. Only the two fields that describe an image rather than being the image
/// are changeable; replacing the picture means uploading a new one.
struct GameMediaUpdate {
    std::optional<std::string> altText;
    std::optional<int> sortOrder;

    bool empty() const { return !altText && !sortOrder; }
};

} // namespace launcher::domain
