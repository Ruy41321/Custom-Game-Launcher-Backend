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
enum class MediaKind { Cover, Banner, Logo, Screenshot, Video };

const char* toString(MediaKind value);

std::optional<MediaKind> parseMediaKind(std::string_view value);

/// True for the kinds a game has at most one of.
bool isSingletonKind(MediaKind kind);

/// True for the one kind that carries a moving picture rather than a still one.
///
/// It decides which size limit, which sniffer and which gallery cap apply, so it is asked once
/// in the service rather than spelled out as `kind == MediaKind::Video` at each of the three.
bool isVideoKind(MediaKind kind);

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

/// The video containers this server is willing to store and to serve.
///
/// Two, and no more, because every one of them is a public `Content-Type` and a format the
/// launcher has to be able to play. MP4 and WebM are what LibVLC and every browser agree on;
/// Matroska, QuickTime and AVI are all playable somewhere and none of them everywhere, which is
/// the property that matters for bytes a publisher uploads once and everybody downloads.
enum class VideoFormat { Mp4, WebM };

const char* contentTypeOf(VideoFormat format);

const char* extensionOf(VideoFormat format);

/// What the storage layer needs to know about a format, so it does not need to know which of
/// the two enums the caller had.
///
/// Both members point at string literals owned by this translation unit — the same ones
/// `contentTypeOf` and `extensionOf` return — so this is a view, not an owner, and it never
/// outlives the program.
struct StoredFormat {
    const char* contentType;
    const char* extension;
};

StoredFormat storedFormatOf(ImageFormat format);

StoredFormat storedFormatOf(VideoFormat format);

/// Identifies an image by its leading bytes.
///
/// The uploader's `Content-Type` header is not consulted at all. What a client claims about a
/// body it controls decides nothing here, precisely because the answer ends up as the
/// `Content-Type` of a public URL.
std::optional<ImageFormat> sniffImageFormat(std::string_view bytes);

/// Identifies a video container by its leading bytes, for the reason `sniffImageFormat` gives
/// and one more: a container is much harder to recognise than a PNG, and recognising it is not
/// the same as vouching for the streams inside it.
///
/// What this answers is "which of the two containers is this", never "is this a playable
/// video". An MP4 whose header is well formed and whose payload is nonsense passes here and
/// fails in the player, and that is the honest division of labour: the server is deciding what
/// `Content-Type` a public URL will announce, not decoding anything.
///
/// The ISO base media brand is checked and not just the `ftyp` box, because HEIC and AVIF are
/// ISO base media files too — accepting the box alone would file a still image as `video/mp4`.
std::optional<VideoFormat> sniffVideoFormat(std::string_view bytes);

inline constexpr std::size_t MAX_ALT_TEXT_LENGTH = 300;

/// A gallery, not an archive. The cap exists so one game cannot fill the media volume, and it
/// is checked before any byte is written.
inline constexpr int MAX_SCREENSHOTS_PER_GAME = 12;

/// Videos are capped far lower than screenshots, and separately, because one of them is worth
/// more than the whole gallery of stills: at the shipped limits three videos are 192 MiB and
/// twelve screenshots are 60. A trailer and a clip or two is what a game page is for; a channel
/// is not.
inline constexpr int MAX_VIDEOS_PER_GAME = 3;

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
