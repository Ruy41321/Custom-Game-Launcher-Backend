#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "domain/Catalog.h"
#include "domain/Manifest.h"
#include "domain/Semver.h"
#include "domain/Validation.h"

namespace {

using launcher::common::ErrorCode;
using launcher::domain::canonicalManifestDocument;
using launcher::domain::isSha256Hex;
using launcher::domain::isUuid;
using launcher::domain::ManifestEntry;
using launcher::domain::parseSemver;
using launcher::domain::slugify;
using launcher::domain::validateManifest;
using launcher::domain::validateRelativePath;
using launcher::domain::validateReleaseDate;
using launcher::domain::validateSlug;

std::string sha(char filler) {
    return std::string(64, filler);
}

// ---------------------------------------------------------------------------
// Version parsing
// ---------------------------------------------------------------------------

TEST(SemverTest, AcceptsOneTwoAndThreeComponents) {
    const auto one = parseSemver("2");
    const auto two = parseSemver("2.5");
    const auto three = parseSemver("0.10.3");

    ASSERT_TRUE(one.ok());
    EXPECT_EQ(one.value().major, 2);
    EXPECT_EQ(one.value().minor, 0);

    ASSERT_TRUE(two.ok());
    EXPECT_EQ(two.value().minor, 5);

    ASSERT_TRUE(three.ok());
    EXPECT_EQ(three.value().major, 0);
    EXPECT_EQ(three.value().minor, 10);
    EXPECT_EQ(three.value().patch, 3);
    EXPECT_EQ(three.value().text, "0.10.3");
}

// The components are stored separately precisely so this ordering is numeric; if the parse
// did not produce them, 0.10.0 would sort before 0.9.0 everywhere.
TEST(SemverTest, ParsesComponentsSoThatTenSortsAfterNine) {
    const auto nine = parseSemver("0.9.0");
    const auto ten = parseSemver("0.10.0");

    ASSERT_TRUE(nine.ok());
    ASSERT_TRUE(ten.ok());
    EXPECT_LT(nine.value().minor, ten.value().minor);
    EXPECT_GT(std::string("0.9.0"), std::string("0.10.0")) << "text ordering is the wrong one";
}

TEST(SemverTest, RejectsMalformedVersions) {
    for (const auto* input : {"", "  ", "1.2.3.4", "1..2", "v1.2", "1.2.x", "-1", "1.2.3-beta"}) {
        const auto parsed = parseSemver(input);
        EXPECT_FALSE(parsed.ok()) << "accepted: " << input;
    }
}

// Otherwise 1.01 and 1.1 could coexist as two versions of the same game.
TEST(SemverTest, RejectsLeadingZeros) {
    EXPECT_FALSE(parseSemver("1.01").ok());
    EXPECT_TRUE(parseSemver("1.0.1").ok());
}

TEST(SemverTest, RejectsComponentsThatAreTooLarge) {
    EXPECT_FALSE(parseSemver("1000000").ok());
    EXPECT_TRUE(parseSemver("999999").ok());
}

// ---------------------------------------------------------------------------
// Slugs and dates
// ---------------------------------------------------------------------------

TEST(SlugTest, AcceptsLowercaseHyphenatedSlugs) {
    EXPECT_TRUE(validateSlug("my-great-game").ok());
    EXPECT_TRUE(validateSlug("game2").ok());
}

TEST(SlugTest, RejectsAnythingThatWouldNotSurviveAUrl) {
    for (const auto* input : {"",
                              "-lead",
                              "trail-",
                              "double--hyphen",
                              "Upper",
                              "with space",
                              "with_underscore",
                              "slash/es"}) {
        EXPECT_FALSE(validateSlug(input).ok()) << "accepted: " << input;
    }
}

TEST(SlugTest, DerivesASlugFromATitle) {
    EXPECT_EQ(slugify("My Great Game!"), "my-great-game");
    EXPECT_EQ(slugify("  Spaced   Out  "), "spaced-out");
    EXPECT_EQ(slugify("Half-Life 2"), "half-life-2");
    EXPECT_TRUE(validateSlug(slugify("Tricky: A Game?!")).ok());
}

TEST(SlugTest, ProducesAnEmptySlugWhenATitleHasNothingUsable) {
    EXPECT_TRUE(slugify("!!!").empty());
}

TEST(ReleaseDateTest, AcceptsRealCalendarDatesOnly) {
    EXPECT_TRUE(validateReleaseDate("2026-04-30").ok());
    EXPECT_TRUE(validateReleaseDate("2024-02-29").ok()) << "2024 is a leap year";

    for (const auto* input :
         {"2026-4-30", "30-04-2026", "2026-13-01", "2026-02-30", "2023-02-29", "not-a-date", ""}) {
        EXPECT_FALSE(validateReleaseDate(input).ok()) << "accepted: " << input;
    }
}

TEST(UuidTest, RecognisesTheCanonicalForm) {
    EXPECT_TRUE(isUuid("0f8fad5b-d9cb-469f-a165-70867728950e"));
    EXPECT_FALSE(isUuid("0f8fad5bd9cb469fa16570867728950e"));
    EXPECT_FALSE(isUuid("not-a-uuid"));
    EXPECT_FALSE(isUuid(""));
    EXPECT_FALSE(isUuid("0f8fad5b-d9cb-469f-a165-70867728950z"));
}

// ---------------------------------------------------------------------------
// Manifest paths — the traversal defence
// ---------------------------------------------------------------------------

TEST(ManifestPathTest, AcceptsOrdinaryRelativePaths) {
    EXPECT_TRUE(validateRelativePath("Game.exe").ok());
    EXPECT_TRUE(validateRelativePath("data/textures/hero.png").ok());
    EXPECT_TRUE(validateRelativePath("a.b/c-d_e/f g.txt").ok());
}

// A manifest is written by a publisher and applied by a client to its own disk, so anything
// that could escape the install directory has to be refused before it is ever stored.
TEST(ManifestPathTest, RejectsEveryFormOfEscape) {
    for (const auto* input : {"",
                              "/etc/passwd",
                              "../secrets",
                              "data/../../secrets",
                              "data/..",
                              "..",
                              ".",
                              "./relative",
                              "data//double",
                              "trailing/",
                              "windows\\path",
                              "C:/absolute",
                              "c:relative"}) {
        EXPECT_FALSE(validateRelativePath(input).ok()) << "accepted: " << input;
    }
}

TEST(ManifestPathTest, RejectsControlCharacters) {
    EXPECT_FALSE(validateRelativePath(std::string("bad\nname")).ok());
    EXPECT_FALSE(validateRelativePath(std::string("bad\0name", 8)).ok());
}

TEST(ManifestPathTest, RejectsPathsPastTheLengthLimit) {
    EXPECT_FALSE(validateRelativePath(std::string(2000, 'a')).ok());
}

TEST(Sha256Test, AcceptsOnlyLowercaseHexOfTheRightLength) {
    EXPECT_TRUE(isSha256Hex(sha('a')));
    EXPECT_FALSE(isSha256Hex(sha('A'))) << "uppercase would produce a second address for one blob";
    EXPECT_FALSE(isSha256Hex(std::string(63, 'a')));
    EXPECT_FALSE(isSha256Hex(sha('g')));
    EXPECT_FALSE(isSha256Hex(""));
}

// ---------------------------------------------------------------------------
// Manifest validation and canonical form
// ---------------------------------------------------------------------------

std::vector<ManifestEntry> sampleFiles() {
    return {ManifestEntry{"Game.exe", sha('a'), 10, true},
            ManifestEntry{"data/pack.bin", sha('b'), 20, false}};
}

TEST(ManifestTest, AcceptsAWellFormedFileList) {
    EXPECT_TRUE(validateManifest(sampleFiles(), "Game.exe").ok());
}

TEST(ManifestTest, RejectsAnEmptyManifest) {
    EXPECT_FALSE(validateManifest({}, "Game.exe").ok());
}

TEST(ManifestTest, RejectsAnEntrypointThatIsNotInTheManifest) {
    const auto result = validateManifest(sampleFiles(), "Missing.exe");
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
}

// Either duplicate could be the one the publisher meant, so this is a conflict, not a merge.
TEST(ManifestTest, RejectsDuplicatePaths) {
    auto files = sampleFiles();
    files.push_back(ManifestEntry{"Game.exe", sha('c'), 30, false});

    const auto result = validateManifest(files, "Game.exe");
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::Conflict);
}

TEST(ManifestTest, RejectsABadHashOrNegativeSize) {
    auto badHash = sampleFiles();
    badHash[0].blobSha256 = "not-a-hash";
    EXPECT_FALSE(validateManifest(badHash, "Game.exe").ok());

    auto badSize = sampleFiles();
    badSize[0].sizeBytes = -1;
    EXPECT_FALSE(validateManifest(badSize, "Game.exe").ok());
}

// The canonical document is a wire contract: the client hashes exactly these bytes to verify
// what it downloaded, so the ordering and shape must not depend on how the list arrived.
TEST(ManifestTest, CanonicalFormIsIndependentOfInputOrder) {
    auto forward = sampleFiles();
    auto reversed = sampleFiles();
    std::reverse(reversed.begin(), reversed.end());

    EXPECT_EQ(canonicalManifestDocument(forward, "Game.exe", ""),
              canonicalManifestDocument(reversed, "Game.exe", ""));
}

TEST(ManifestTest, CanonicalFormHasNoWhitespaceAndAFixedKeyOrder) {
    const auto document = canonicalManifestDocument(
        {ManifestEntry{"Game.exe", sha('a'), 10, true}}, "Game.exe", "-w");

    EXPECT_EQ(document,
              R"({"schema":1,"entrypoint":"Game.exe","launchArgs":"-w","files":[{"path":)"
              R"("Game.exe","sha256":")" +
                  sha('a') + R"(","size":10,"executable":true}]})");
}

TEST(ManifestTest, CanonicalFormEscapesPathsThatWouldBreakTheDocument) {
    const auto document = canonicalManifestDocument(
        {ManifestEntry{"quote\"and\\slash", sha('a'), 1, false}}, "quote\"and\\slash", "");

    EXPECT_NE(document.find(R"(quote\"and\\slash)"), std::string::npos);
}

// Two builds with identical content must carry identical manifest hashes, which is what makes
// the hash usable as a content identity — so nothing build-specific may leak into it.
TEST(ManifestTest, CanonicalFormDependsOnlyOnContent) {
    const auto first = canonicalManifestDocument(sampleFiles(), "Game.exe", "--fullscreen");
    const auto second = canonicalManifestDocument(sampleFiles(), "Game.exe", "--fullscreen");
    const auto other = canonicalManifestDocument(sampleFiles(), "Game.exe", "--windowed");

    EXPECT_EQ(first, second);
    EXPECT_NE(first, other) << "launch arguments are part of what the hash covers";
}

} // namespace
