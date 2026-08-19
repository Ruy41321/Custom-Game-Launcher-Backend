#include "domain/Media.h"

#include <string>

#include "domain/ValidationRules.h"

namespace launcher::domain {
namespace {

using common::invalidInput;
using common::VoidResult;

bool startsWith(std::string_view bytes, std::string_view prefix) {
    return bytes.size() >= prefix.size() && bytes.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

const char* toString(MediaKind value) {
    switch (value) {
    case MediaKind::Cover:
        return "cover";
    case MediaKind::Banner:
        return "banner";
    case MediaKind::Logo:
        return "logo";
    case MediaKind::Video:
        return "video";
    case MediaKind::Screenshot:
        break;
    }
    return "screenshot";
}

std::optional<MediaKind> parseMediaKind(std::string_view value) {
    if (value == "cover") {
        return MediaKind::Cover;
    }
    if (value == "banner") {
        return MediaKind::Banner;
    }
    if (value == "logo") {
        return MediaKind::Logo;
    }
    if (value == "screenshot") {
        return MediaKind::Screenshot;
    }
    if (value == "video") {
        return MediaKind::Video;
    }
    return std::nullopt;
}

bool isSingletonKind(MediaKind kind) {
    return kind == MediaKind::Cover || kind == MediaKind::Banner || kind == MediaKind::Logo;
}

bool isVideoKind(MediaKind kind) {
    return kind == MediaKind::Video;
}

const char* contentTypeOf(ImageFormat format) {
    switch (format) {
    case ImageFormat::Jpeg:
        return "image/jpeg";
    case ImageFormat::WebP:
        return "image/webp";
    case ImageFormat::Png:
        break;
    }
    return "image/png";
}

const char* extensionOf(ImageFormat format) {
    switch (format) {
    case ImageFormat::Jpeg:
        return "jpg";
    case ImageFormat::WebP:
        return "webp";
    case ImageFormat::Png:
        break;
    }
    return "png";
}

const char* contentTypeOf(VideoFormat format) {
    switch (format) {
    case VideoFormat::WebM:
        return "video/webm";
    case VideoFormat::Mp4:
        break;
    }
    return "video/mp4";
}

const char* extensionOf(VideoFormat format) {
    switch (format) {
    case VideoFormat::WebM:
        return "webm";
    case VideoFormat::Mp4:
        break;
    }
    return "mp4";
}

StoredFormat storedFormatOf(ImageFormat format) {
    return StoredFormat{contentTypeOf(format), extensionOf(format)};
}

StoredFormat storedFormatOf(VideoFormat format) {
    return StoredFormat{contentTypeOf(format), extensionOf(format)};
}

std::optional<ImageFormat> sniffImageFormat(std::string_view bytes) {
    static constexpr std::string_view PNG_SIGNATURE("\x89PNG\r\n\x1a\n", 8);
    if (startsWith(bytes, PNG_SIGNATURE)) {
        return ImageFormat::Png;
    }

    // Every JPEG variant this cares about opens with SOI followed by a marker.
    static constexpr std::string_view JPEG_SIGNATURE("\xff\xd8\xff", 3);
    if (startsWith(bytes, JPEG_SIGNATURE)) {
        return ImageFormat::Jpeg;
    }

    // RIFF containers name their payload at offset 8, so "RIFF....WEBP" is the whole check;
    // matching "RIFF" alone would accept a WAV file.
    if (bytes.size() >= 12 && startsWith(bytes, "RIFF") && bytes.compare(8, 4, "WEBP") == 0) {
        return ImageFormat::WebP;
    }

    return std::nullopt;
}

std::optional<VideoFormat> sniffVideoFormat(std::string_view bytes) {
    // Matroska and WebM share the EBML header; what tells them apart is the DocType, a string
    // element inside it. Reading it properly means parsing variable-length EBML integers to
    // find one element, and the whole header is a few dozen bytes, so the check is: the magic,
    // then "webm" somewhere in the region the DocType has to be in. A Matroska file says
    // "matroska" there and is refused, which is the distinction that had to be drawn.
    static constexpr std::string_view EBML_SIGNATURE("\x1a\x45\xdf\xa3", 4);
    static constexpr std::size_t EBML_HEADER_WINDOW = 64;
    if (startsWith(bytes, EBML_SIGNATURE)) {
        const auto window = bytes.substr(0, EBML_HEADER_WINDOW);
        if (window.find("webm") != std::string_view::npos) {
            return VideoFormat::WebM;
        }
        return std::nullopt;
    }

    // ISO base media: a box whose type is "ftyp" at offset 4, and its major brand at offset 8.
    // The brand is the whole point of looking — HEIC, AVIF and QuickTime are all ISO base media
    // files, and only some of them are video this server would be right to announce as MP4.
    if (bytes.size() < 12 || bytes.compare(4, 4, "ftyp") != 0) {
        return std::nullopt;
    }
    const auto brand = bytes.substr(8, 4);
    // The brands an encoder anybody uses actually writes for MP4. "M4V " is Apple's video
    // profile of the same container; "qt  " is QuickTime and is deliberately absent, because a
    // .mov is not what `video/mp4` promises even where a player happens to open it.
    for (const std::string_view accepted :
         {"isom", "iso2", "iso4", "iso5", "iso6", "mp41", "mp42", "avc1", "dash", "M4V "}) {
        if (brand == accepted) {
            return VideoFormat::Mp4;
        }
    }
    return std::nullopt;
}

VoidResult validateAltText(std::string_view altText) {
    if (altText.size() > MAX_ALT_TEXT_LENGTH) {
        return VoidResult::failure(invalidInput(
            "altText must be at most " + std::to_string(MAX_ALT_TEXT_LENGTH) + " characters",
            rules::ALT_TEXT_TOO_LONG,
            {std::to_string(MAX_ALT_TEXT_LENGTH)}));
    }
    for (const char character : altText) {
        const auto value = static_cast<unsigned char>(character);
        if (value < 0x20 && character != '\n' && character != '\t') {
            return VoidResult::failure(invalidInput("altText must not contain control characters",
                                                    rules::ALT_TEXT_INVALID));
        }
    }
    return VoidResult::success();
}

} // namespace launcher::domain
