#include "domain/Media.h"

namespace launcher::domain {
namespace {

using common::ErrorCode;
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
    return std::nullopt;
}

bool isSingletonKind(MediaKind kind) {
    return kind != MediaKind::Screenshot;
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

VoidResult validateAltText(std::string_view altText) {
    if (altText.size() > MAX_ALT_TEXT_LENGTH) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "altText must be at most " +
                                       std::to_string(MAX_ALT_TEXT_LENGTH) + " characters");
    }
    for (const char character : altText) {
        const auto value = static_cast<unsigned char>(character);
        if (value < 0x20 && character != '\n' && character != '\t') {
            return VoidResult::failure(ErrorCode::InvalidInput,
                                       "altText must not contain control characters");
        }
    }
    return VoidResult::success();
}

} // namespace launcher::domain
