#include <gtest/gtest.h>

#include <string>

#include "app/ReleaseDocumentJson.h"
#include "domain/LauncherRelease.h"

namespace {

using launcher::app::parseReleaseDocument;
using launcher::domain::canonicalReleaseDocument;
using launcher::domain::ReleaseArch;
using launcher::domain::ReleaseChannel;
using launcher::domain::ReleasePlatform;

constexpr const char* CANONICAL =
    R"({"schema":1,"channel":"stable","version":"0.2.0","platform":"windows","arch":"x64",)"
    R"("sha256":"9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",)"
    R"("size":83442176,"releasedAt":"2026-08-07T10:00:00Z","notes":"Self-update, at last."})";

TEST(ReleaseDocumentJsonTest, ReadsTheCanonicalForm) {
    const auto parsed = parseReleaseDocument(CANONICAL);
    ASSERT_TRUE(parsed.ok()) << parsed.error().detail;

    const auto& document = parsed.value();
    EXPECT_EQ(document.channel, ReleaseChannel::Stable);
    EXPECT_EQ(document.platform, ReleasePlatform::Windows);
    EXPECT_EQ(document.arch, ReleaseArch::X64);
    EXPECT_EQ(document.version.text, "0.2.0");
    EXPECT_EQ(document.version.minor, 2);
    EXPECT_EQ(document.artifactSize, 83442176);
    EXPECT_EQ(document.notes, "Self-update, at last.");
}

TEST(ReleaseDocumentJsonTest, WhatItReadsIsWhatItWouldHaveWritten) {
    const auto parsed = parseReleaseDocument(CANONICAL);
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(canonicalReleaseDocument(parsed.value()), CANONICAL);
}

// The round-trip check, which is the whole reason this function exists rather than a bare
// jsoncpp parse. Each of these is valid JSON meaning the right thing, and each would leave the
// stored bytes saying something the derived columns beside them do not.
TEST(ReleaseDocumentJsonTest, RefusesAnythingThatIsNotByteForByteCanonical) {
    // Pretty-printed.
    EXPECT_FALSE(parseReleaseDocument(std::string(R"({ "schema": 1, "channel": "stable" })")).ok());

    // Keys reordered, everything else identical.
    EXPECT_FALSE(
        parseReleaseDocument(
            R"({"channel":"stable","schema":1,"version":"0.2.0","platform":"windows",)"
            R"("arch":"x64",)"
            R"("sha256":"9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",)"
            R"("size":83442176,"releasedAt":"2026-08-07T10:00:00Z",)"
            R"("notes":"Self-update, at last."})")
            .ok());

    // An extra key the parse ignores. Without the round-trip check this would be stored, and
    // whatever it said would be inside the signature and outside everything that reads it.
    std::string extra = CANONICAL;
    extra.insert(extra.size() - 1, R"(,"minimumVersion":"9.9.9")");
    EXPECT_FALSE(parseReleaseDocument(extra).ok());

    // One trailing newline, which is what a text editor adds on save.
    EXPECT_FALSE(parseReleaseDocument(std::string(CANONICAL) + "\n").ok());
}

TEST(ReleaseDocumentJsonTest, RefusesWhatIsNotADocumentAtAll) {
    EXPECT_FALSE(parseReleaseDocument("").ok());
    EXPECT_FALSE(parseReleaseDocument("not json").ok());
    EXPECT_FALSE(parseReleaseDocument("[]").ok());
    EXPECT_FALSE(parseReleaseDocument("{}").ok());
}

TEST(ReleaseDocumentJsonTest, RefusesUnknownChannelsPlatformsAndArchitectures) {
    const auto swap = [](std::string_view from, std::string_view to) {
        std::string document = CANONICAL;
        document.replace(document.find(from), from.size(), to);
        return document;
    };

    EXPECT_FALSE(
        parseReleaseDocument(swap(R"("channel":"stable")", R"("channel":"nightly")")).ok());
    EXPECT_FALSE(parseReleaseDocument(swap(R"("platform":"windows")", R"("platform":"osx")")).ok());
    EXPECT_FALSE(parseReleaseDocument(swap(R"("arch":"x64")", R"("arch":"amd64")")).ok());
}

TEST(ReleaseDocumentJsonTest, RefusesASizeThatIsNotAWholeNumber) {
    std::string document = CANONICAL;
    document.replace(document.find("83442176"), 8, "1.5");
    EXPECT_FALSE(parseReleaseDocument(document).ok());
}

// The refusal carries the form that was expected, because the person reading it is an operator
// with a shell and a file they are about to fix.
TEST(ReleaseDocumentJsonTest, TheRefusalShowsTheCanonicalForm) {
    const auto refused = parseReleaseDocument(std::string(CANONICAL) + "\n");
    ASSERT_FALSE(refused.ok());
    EXPECT_NE(refused.error().detail.find(CANONICAL), std::string::npos);
}

} // namespace
