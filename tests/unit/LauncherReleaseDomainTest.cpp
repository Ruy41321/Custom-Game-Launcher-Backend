#include <gtest/gtest.h>

#include <string>

#include "domain/LauncherRelease.h"
#include "domain/Semver.h"

namespace {

using launcher::domain::canonicalReleaseDocument;
using launcher::domain::isNewerRelease;
using launcher::domain::parseReleaseArch;
using launcher::domain::parseReleaseChannel;
using launcher::domain::parseReleasePlatform;
using launcher::domain::ReleaseArch;
using launcher::domain::ReleaseChannel;
using launcher::domain::ReleaseDocument;
using launcher::domain::ReleasePlatform;
using launcher::domain::Semver;
using launcher::domain::validateReleaseDocument;

constexpr const char* VALID_SHA =
    "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";

ReleaseDocument validDocument() {
    ReleaseDocument document;
    document.channel = ReleaseChannel::Stable;
    document.version = Semver{0, 2, 0, "0.2.0"};
    document.platform = ReleasePlatform::Windows;
    document.arch = ReleaseArch::X64;
    document.artifactSha256 = VALID_SHA;
    document.artifactSize = 83442176;
    document.releasedAt = "2026-08-07T10:00:00Z";
    document.notes = "Self-update, at last.";
    return document;
}

// The bytes a signature covers, spelled out. If this assertion ever has to change, every
// signature ever produced stops verifying — which is exactly why it is written as a literal
// rather than as a round trip through the serialiser under test.
TEST(LauncherReleaseDomainTest, TheCanonicalDocumentIsExactlyTheseBytes) {
    EXPECT_EQ(canonicalReleaseDocument(validDocument()),
              R"({"schema":1,"channel":"stable","version":"0.2.0","platform":"windows",)"
              R"("arch":"x64",)"
              R"("sha256":"9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",)"
              R"("size":83442176,"releasedAt":"2026-08-07T10:00:00Z",)"
              R"("notes":"Self-update, at last."})");
}

TEST(LauncherReleaseDomainTest, EscapesNotesRatherThanEmittingThemRaw) {
    auto document = validDocument();
    // Built from a code point rather than written into the literal: a raw control
    // character in a source file is invisible in a diff and tests the wrong thing.
    document.notes = std::string("a \"quoted\" line\nand a tab\t") + '\x01';

    EXPECT_NE(canonicalReleaseDocument(document).find(
                  R"("notes":"a \"quoted\" line\nand a tab\t\u0001"})"),
              std::string::npos);
}

TEST(LauncherReleaseDomainTest, AcceptsADocumentThatCouldHaveBeenMeant) {
    EXPECT_TRUE(validateReleaseDocument(validDocument()).ok());
}

// `0.2` and `0.2.0` are one version written two ways, and the unique index sees them as two
// rows racing to be the newest.
TEST(LauncherReleaseDomainTest, RefusesAVersionThatIsNotThreeComponents) {
    auto document = validDocument();
    document.version = Semver{0, 2, 0, "0.2"};
    EXPECT_FALSE(validateReleaseDocument(document).ok());

    document.version = Semver{1, 0, 0, "1"};
    EXPECT_FALSE(validateReleaseDocument(document).ok());
}

TEST(LauncherReleaseDomainTest, RefusesAHashThatIsNotOne) {
    auto document = validDocument();
    document.artifactSha256 = "not a hash";
    EXPECT_FALSE(validateReleaseDocument(document).ok());

    // Right length, wrong alphabet — the shape a filler string takes.
    document.artifactSha256 = std::string(64, 'x');
    EXPECT_FALSE(validateReleaseDocument(document).ok());

    // Uppercase is a different string, and would be a second content address for one file.
    document.artifactSha256 = std::string(64, 'A');
    EXPECT_FALSE(validateReleaseDocument(document).ok());
}

TEST(LauncherReleaseDomainTest, RefusesASizeThatIsNotAnArtifact) {
    auto document = validDocument();
    document.artifactSize = 0;
    EXPECT_FALSE(validateReleaseDocument(document).ok());

    document.artifactSize = -1;
    EXPECT_FALSE(validateReleaseDocument(document).ok());

    document.artifactSize = launcher::domain::MAX_RELEASE_ARTIFACT_BYTES + 1;
    EXPECT_FALSE(validateReleaseDocument(document).ok());
}

// A signature covers bytes, so an instant that can be spelled several ways is several documents.
TEST(LauncherReleaseDomainTest, RefusesATimestampThatIsNotTheOneSpelling) {
    auto document = validDocument();
    for (const auto* stamp : {"2026-08-07T10:00:00+02:00",
                              "2026-08-07T10:00:00.500Z",
                              "2026-08-07 10:00:00Z",
                              "2026-08-07T10:00:00",
                              "2026-13-07T10:00:00Z",
                              "2026-08-07T25:00:00Z",
                              ""}) {
        document.releasedAt = stamp;
        EXPECT_FALSE(validateReleaseDocument(document).ok()) << stamp;
    }
}

TEST(LauncherReleaseDomainTest, RefusesTheWrongSchema) {
    auto document = validDocument();
    document.schema = 2;
    EXPECT_FALSE(validateReleaseDocument(document).ok());
}

TEST(LauncherReleaseDomainTest, RefusesNotesLongerThanAParagraph) {
    auto document = validDocument();
    document.notes = std::string(launcher::domain::MAX_LAUNCHER_RELEASE_NOTES_LENGTH + 1, 'a');
    EXPECT_FALSE(validateReleaseDocument(document).ok());
}

// Compared as text, 0.10.0 sorts before 0.9.0 — which would offer a downgrade as the newest
// release and, on the client, be refused as older than what is installed. Both halves of that
// are this comparison.
TEST(LauncherReleaseDomainTest, OrdersVersionsNumericallyAndStrictly) {
    const Semver nine{0, 9, 0, "0.9.0"};
    const Semver ten{0, 10, 0, "0.10.0"};

    EXPECT_TRUE(isNewerRelease(ten, nine));
    EXPECT_FALSE(isNewerRelease(nine, ten));

    // Strict: the same version is not an update, which is what makes a correctly signed but
    // replayed document useless to whoever replayed it.
    EXPECT_FALSE(isNewerRelease(nine, nine));

    EXPECT_TRUE(isNewerRelease(Semver{1, 0, 0, "1.0.0"}, Semver{0, 99, 99, "0.99.99"}));
    EXPECT_TRUE(isNewerRelease(Semver{0, 2, 1, "0.2.1"}, Semver{0, 2, 0, "0.2.0"}));
}

TEST(LauncherReleaseDomainTest, ParsesOnlyTheNamesItSerialises) {
    EXPECT_EQ(parseReleaseChannel("stable"), ReleaseChannel::Stable);
    EXPECT_EQ(parseReleaseChannel("beta"), ReleaseChannel::Beta);
    EXPECT_FALSE(parseReleaseChannel("STABLE").has_value());
    EXPECT_FALSE(parseReleaseChannel("nightly").has_value());
    EXPECT_FALSE(parseReleaseChannel("").has_value());

    EXPECT_EQ(parseReleasePlatform("windows"), ReleasePlatform::Windows);
    EXPECT_EQ(parseReleasePlatform("linux"), ReleasePlatform::Linux);
    EXPECT_EQ(parseReleasePlatform("macos"), ReleasePlatform::MacOS);
    // The runtime identifier, not the platform name: a real mistake to make, and one that must
    // not silently resolve to something.
    EXPECT_FALSE(parseReleasePlatform("osx").has_value());
    EXPECT_FALSE(parseReleasePlatform("win").has_value());

    EXPECT_EQ(parseReleaseArch("x64"), ReleaseArch::X64);
    EXPECT_EQ(parseReleaseArch("arm64"), ReleaseArch::Arm64);
    EXPECT_FALSE(parseReleaseArch("amd64").has_value());
}

// Every name that is serialised parses back, which is what keeps the canonical document and the
// route's query string talking about the same set.
TEST(LauncherReleaseDomainTest, EveryNameRoundTrips) {
    for (const auto channel : {ReleaseChannel::Stable, ReleaseChannel::Beta}) {
        EXPECT_EQ(parseReleaseChannel(launcher::domain::nameFor(channel)), channel);
    }
    for (const auto platform :
         {ReleasePlatform::Windows, ReleasePlatform::Linux, ReleasePlatform::MacOS}) {
        EXPECT_EQ(parseReleasePlatform(launcher::domain::nameFor(platform)), platform);
    }
    for (const auto arch : {ReleaseArch::X64, ReleaseArch::Arm64}) {
        EXPECT_EQ(parseReleaseArch(launcher::domain::nameFor(arch)), arch);
    }
}

} // namespace
