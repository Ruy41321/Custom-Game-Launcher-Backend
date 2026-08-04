#include <gtest/gtest.h>

#include <string>

#include "domain/Media.h"

namespace {

using launcher::domain::ImageFormat;
using launcher::domain::MediaKind;

/// Builds a body that begins with the given signature and then carries filler, because a real
/// image is never only its magic number and the sniffer must not care what follows.
std::string bodyStartingWith(const std::string& signature) {
    return signature + std::string(64, '\x01');
}

const std::string PNG("\x89PNG\r\n\x1a\n", 8);
const std::string JPEG("\xff\xd8\xff\xe0", 4);

TEST(MediaKindTest, RoundTripsEverySpelling) {
    for (const auto kind :
         {MediaKind::Cover, MediaKind::Banner, MediaKind::Logo, MediaKind::Screenshot}) {
        const auto parsed = launcher::domain::parseMediaKind(launcher::domain::toString(kind));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, kind);
    }
}

TEST(MediaKindTest, RejectsAnythingElse) {
    EXPECT_FALSE(launcher::domain::parseMediaKind("").has_value());
    EXPECT_FALSE(launcher::domain::parseMediaKind("Cover").has_value());
    EXPECT_FALSE(launcher::domain::parseMediaKind("thumbnail").has_value());
}

TEST(MediaKindTest, OnlyScreenshotsAreAGallery) {
    EXPECT_TRUE(launcher::domain::isSingletonKind(MediaKind::Cover));
    EXPECT_TRUE(launcher::domain::isSingletonKind(MediaKind::Banner));
    EXPECT_TRUE(launcher::domain::isSingletonKind(MediaKind::Logo));
    EXPECT_FALSE(launcher::domain::isSingletonKind(MediaKind::Screenshot));
}

TEST(ImageSniffTest, RecognisesThePngSignature) {
    const auto format = launcher::domain::sniffImageFormat(bodyStartingWith(PNG));
    ASSERT_TRUE(format.has_value());
    EXPECT_EQ(*format, ImageFormat::Png);
}

TEST(ImageSniffTest, RecognisesTheJpegSignature) {
    const auto format = launcher::domain::sniffImageFormat(bodyStartingWith(JPEG));
    ASSERT_TRUE(format.has_value());
    EXPECT_EQ(*format, ImageFormat::Jpeg);
}

/// A RIFF header carries a little-endian length whose high bytes are NUL, so these bodies have
/// to be built with an explicit size: from a bare string literal a string_view stops at the
/// first NUL and the sniffer is handed five bytes instead of sixteen.
std::string riffBody(const std::string& payloadTag) {
    return std::string("RIFF\x24\x00\x00\x00", 8) + payloadTag + "trailing bytes";
}

TEST(ImageSniffTest, RecognisesAWebPRiffContainer) {
    const auto format = launcher::domain::sniffImageFormat(riffBody("WEBP"));
    ASSERT_TRUE(format.has_value());
    EXPECT_EQ(*format, ImageFormat::WebP);
}

TEST(ImageSniffTest, RejectsARiffContainerThatIsNotWebP) {
    // A WAV file is also RIFF, and matching on "RIFF" alone would store one as a picture.
    EXPECT_FALSE(launcher::domain::sniffImageFormat(riffBody("WAVE")).has_value());
}

TEST(ImageSniffTest, RejectsWhatTheContentTypeHeaderWouldHaveClaimed) {
    // These are the two that matter: media is served from a public URL, so anything that a
    // browser would treat as a document must never acquire an image content type.
    EXPECT_FALSE(launcher::domain::sniffImageFormat("<svg xmlns=\"http://www.w3.org/2000/svg\">")
                     .has_value());
    EXPECT_FALSE(launcher::domain::sniffImageFormat("<!DOCTYPE html><script>").has_value());
}

TEST(ImageSniffTest, RejectsEmptyAndTruncatedBodies) {
    EXPECT_FALSE(launcher::domain::sniffImageFormat("").has_value());
    EXPECT_FALSE(launcher::domain::sniffImageFormat(PNG.substr(0, 4)).has_value());
    // Long enough to be a RIFF header but cut before the payload tag is complete.
    EXPECT_FALSE(
        launcher::domain::sniffImageFormat(std::string("RIFF\x24\x00\x00\x00WEB", 11)).has_value());
}

TEST(ImageFormatTest, EveryFormatHasAContentTypeAndAnExtension) {
    EXPECT_STREQ(launcher::domain::contentTypeOf(ImageFormat::Png), "image/png");
    EXPECT_STREQ(launcher::domain::contentTypeOf(ImageFormat::Jpeg), "image/jpeg");
    EXPECT_STREQ(launcher::domain::contentTypeOf(ImageFormat::WebP), "image/webp");

    // The extension is what makes nginx answer with a usable content type, and the file server
    // only routes these three.
    EXPECT_STREQ(launcher::domain::extensionOf(ImageFormat::Png), "png");
    EXPECT_STREQ(launcher::domain::extensionOf(ImageFormat::Jpeg), "jpg");
    EXPECT_STREQ(launcher::domain::extensionOf(ImageFormat::WebP), "webp");
}

TEST(AltTextTest, AcceptsOrdinaryDescriptions) {
    EXPECT_TRUE(launcher::domain::validateAltText("").ok());
    EXPECT_TRUE(launcher::domain::validateAltText("A knight standing in the rain").ok());
    EXPECT_TRUE(launcher::domain::validateAltText("Two lines\nof description").ok());
}

TEST(AltTextTest, RejectsOverlongTextAndControlCharacters) {
    EXPECT_FALSE(launcher::domain::validateAltText(
                     std::string(launcher::domain::MAX_ALT_TEXT_LENGTH + 1, 'a'))
                     .ok());
    // Built from its code point rather than pasted, so the intent is visible in the source.
    EXPECT_FALSE(
        launcher::domain::validateAltText(std::string("bell") + static_cast<char>(7)).ok());
}

} // namespace
