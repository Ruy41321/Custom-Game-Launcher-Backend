#include <gtest/gtest.h>

#include <string>

#include "domain/Media.h"

namespace {

using launcher::domain::ImageFormat;
using launcher::domain::MediaKind;
using launcher::domain::VideoFormat;

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

// ---------------------------------------------------------------------------
// Video: a fifth kind, two containers, and a sniffer that identifies rather than vouches
// ---------------------------------------------------------------------------

/// The EBML magic every Matroska-family file opens with. WebM and .mkv share it; what tells
/// them apart is the DocType a few bytes later, which is the whole reason the sniffer looks
/// past the signature.
const std::string EBML("\x1a\x45\xdf\xa3", 4);

/// An ISO base media header: a box length, the literal "ftyp", and then the major brand. The
/// length is nonsense on purpose — nothing here parses the box, and a sniffer that only worked
/// on files whose first box length was plausible would be reading more than it admits to.
std::string isoBaseMedia(const std::string& brand) {
    return std::string("\x00\x00\x00\x20", 4) + "ftyp" + brand + std::string(32, '\x00');
}

/// The shape a real WebM header has: the magic, the EBML header elements, and the DocType
/// string sitting inside them.
std::string webmHeader(const std::string& docType) {
    return EBML + std::string("\x01\x00\x00\x00", 4) + "\x42\x82" + docType +
           std::string(48, '\x00');
}

TEST(MediaKindTest, VideoIsAGalleryKindAndNotASingleton) {
    EXPECT_FALSE(launcher::domain::isSingletonKind(MediaKind::Video));
    EXPECT_TRUE(launcher::domain::isVideoKind(MediaKind::Video));

    // The other four are stills, and the answer decides which limit and which sniffer apply.
    for (const auto kind :
         {MediaKind::Cover, MediaKind::Banner, MediaKind::Logo, MediaKind::Screenshot}) {
        EXPECT_FALSE(launcher::domain::isVideoKind(kind));
    }
}

TEST(MediaKindTest, VideoRoundTripsItsSpelling) {
    const auto parsed = launcher::domain::parseMediaKind("video");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, MediaKind::Video);
    EXPECT_STREQ(launcher::domain::toString(MediaKind::Video), "video");
}

TEST(VideoSniffTest, RecognisesTheOrdinaryMp4Brands) {
    for (const std::string brand : {"isom", "iso2", "mp42", "avc1", "M4V "}) {
        const auto format = launcher::domain::sniffVideoFormat(isoBaseMedia(brand));
        ASSERT_TRUE(format.has_value()) << "brand " << brand;
        EXPECT_EQ(*format, VideoFormat::Mp4) << "brand " << brand;
    }
}

/// The reason the brand is read at all rather than the `ftyp` box alone: these are ISO base
/// media files too, and filing a still picture as `video/mp4` would put a content type on a
/// public URL that the bytes do not support.
TEST(VideoSniffTest, RejectsTheIsoBaseMediaFilesThatAreNotVideo) {
    for (const std::string brand : {"heic", "avif", "mif1", "qt  ", "3gp4"}) {
        EXPECT_FALSE(launcher::domain::sniffVideoFormat(isoBaseMedia(brand)).has_value())
            << "brand " << brand;
    }
}

TEST(VideoSniffTest, RecognisesWebMByItsDocType) {
    const auto format = launcher::domain::sniffVideoFormat(webmHeader("webm"));
    ASSERT_TRUE(format.has_value());
    EXPECT_EQ(*format, VideoFormat::WebM);
}

/// Matroska opens with the same four bytes and is a different container. Accepting it would
/// mean announcing `video/webm` over bytes no browser and not every build of LibVLC will play.
TEST(VideoSniffTest, RejectsMatroskaWhichSharesTheSignature) {
    EXPECT_FALSE(launcher::domain::sniffVideoFormat(webmHeader("matroska")).has_value());
}

/// The DocType lives in the EBML header, at the front. A file that says "webm" a megabyte in
/// says nothing about what it is, and a sniffer that searched the whole body would be reading
/// an attacker's choice of bytes.
TEST(VideoSniffTest, DoesNotLookForTheDocTypePastTheHeader) {
    const auto far = EBML + std::string(200, '\x00') + "webm";
    EXPECT_FALSE(launcher::domain::sniffVideoFormat(far).has_value());
}

TEST(VideoSniffTest, RejectsPicturesAndEmptyAndTruncatedBodies) {
    EXPECT_FALSE(launcher::domain::sniffVideoFormat("").has_value());
    EXPECT_FALSE(launcher::domain::sniffVideoFormat(EBML).has_value());
    EXPECT_FALSE(launcher::domain::sniffVideoFormat(std::string("\x00\x00\x00\x20"
                                                                "ftyp",
                                                                8))
                     .has_value());
    EXPECT_FALSE(
        launcher::domain::sniffVideoFormat(std::string("\x89PNG\r\n\x1a\n", 8) + "a picture")
            .has_value());
    EXPECT_FALSE(launcher::domain::sniffVideoFormat("what the header claimed").has_value());
}

TEST(VideoFormatTest, EveryFormatHasAContentTypeAndAnExtension) {
    EXPECT_STREQ(launcher::domain::contentTypeOf(VideoFormat::Mp4), "video/mp4");
    EXPECT_STREQ(launcher::domain::contentTypeOf(VideoFormat::WebM), "video/webm");

    // The extension is what makes nginx answer with a usable content type, and the file server
    // routes exactly these two on top of the three picture formats.
    EXPECT_STREQ(launcher::domain::extensionOf(VideoFormat::Mp4), "mp4");
    EXPECT_STREQ(launcher::domain::extensionOf(VideoFormat::WebM), "webm");
}

/// `StoredFormat` is what the storage layer is handed, so that one write path serves a picture
/// and a video. If these ever disagreed with the two functions above, a file would be stored
/// under an extension that contradicts the content type recorded next to it.
TEST(VideoFormatTest, TheStoredFormatIsTheSameTwoAnswers) {
    const auto mp4 = launcher::domain::storedFormatOf(VideoFormat::Mp4);
    EXPECT_STREQ(mp4.contentType, "video/mp4");
    EXPECT_STREQ(mp4.extension, "mp4");

    const auto png = launcher::domain::storedFormatOf(launcher::domain::ImageFormat::Png);
    EXPECT_STREQ(png.contentType, "image/png");
    EXPECT_STREQ(png.extension, "png");
}

} // namespace
