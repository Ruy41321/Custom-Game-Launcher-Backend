#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/Result.h"
#include "domain/Actor.h"
#include "domain/Media.h"
#include "domain/Semver.h"

namespace launcher::domain {

/// These mirror the PostgreSQL enums created by migration 0001. The wire representation is
/// the same lowercase string in all three places — database, API and client — so there is
/// exactly one spelling to agree on.
enum class GameVisibility { Draft, Unlisted, Public };
enum class BuildStage { Demo, Alpha, Beta, Release };
enum class BuildPlatform { Windows, Linux, MacOS };
enum class BuildArchitecture { X64, Arm64 };
enum class BuildStatus { Uploading, Ready, Failed };

const char* toString(GameVisibility value);
const char* toString(BuildStage value);
const char* toString(BuildPlatform value);
const char* toString(BuildArchitecture value);
const char* toString(BuildStatus value);

std::optional<GameVisibility> parseGameVisibility(std::string_view value);
std::optional<BuildStage> parseBuildStage(std::string_view value);
std::optional<BuildPlatform> parseBuildPlatform(std::string_view value);
std::optional<BuildArchitecture> parseBuildArchitecture(std::string_view value);
std::optional<BuildStatus> parseBuildStatus(std::string_view value);

inline constexpr std::size_t MAX_SLUG_LENGTH = 80;
inline constexpr std::size_t MAX_TITLE_LENGTH = 200;
inline constexpr std::size_t MAX_SUMMARY_LENGTH = 500;
inline constexpr std::size_t MAX_DESCRIPTION_LENGTH = 20000;
inline constexpr std::size_t MAX_RELEASE_NOTES_LENGTH = 20000;
inline constexpr std::size_t MAX_LAUNCH_ARGS_LENGTH = 1000;

/// Mirrors the games_slug_format CHECK: lowercase alphanumeric groups joined by single
/// hyphens. The slug is part of every catalog URL, so it must stay path-safe.
common::VoidResult validateSlug(std::string_view slug);

/// Derives a slug from a title. Used when a publisher does not supply one; the result is
/// still validated, so a title with no usable characters is an error rather than a surprise.
std::string slugify(std::string_view title);

/// Accepts an ISO `YYYY-MM-DD` calendar date. Validated here rather than left to PostgreSQL
/// so that a typo is a 422 naming the field instead of a driver error.
common::VoidResult validateReleaseDate(std::string_view date);

struct Game {
    std::string id;
    std::string slug;
    std::string title;
    std::string summary;
    std::string description;
    std::string publisherUserId;
    std::string publisherDisplayName;
    std::string releaseDate; ///< `YYYY-MM-DD`, empty when the publisher has not set one
    GameVisibility visibility{GameVisibility::Draft};
    std::string createdAt;
    std::string updatedAt;
    /// Storage key of the cover image, empty when the publisher has not uploaded one. Carried
    /// on the game itself because Explore needs one picture per card and nothing else; the
    /// full media list is only worth a query on the detail page.
    std::string coverStorageKey;
};

struct NewGame {
    std::string slug;
    std::string title;
    std::string summary;
    std::string description;
    std::string publisherUserId;
    std::string releaseDate;
    GameVisibility visibility{GameVisibility::Draft};
};

/// A partial update. An absent field is left untouched; this is what lets the API expose
/// PATCH semantics without a separate "which fields did you mean" flag per column.
struct GameUpdate {
    std::optional<std::string> title;
    std::optional<std::string> summary;
    std::optional<std::string> description;
    std::optional<std::string> releaseDate; ///< empty string clears the date
    std::optional<GameVisibility> visibility;

    bool empty() const { return !title && !summary && !description && !releaseDate && !visibility; }
};

/// May see that a game exists. A draft is visible only to its publisher and to an operator,
/// and to everybody else it is reported missing rather than forbidden — a 403 would confirm
/// an unreleased title exists.
bool mayViewGame(const Game& game, const Actor& actor);

/// May change a game and everything hanging off it: versions, builds, artwork, patch notes.
///
/// Here rather than in a service for the reason D26 gives about builds: more than one service
/// now asks this question, and two copies of an authorization rule is one copy too many.
bool mayEditGame(const Game& game, const Actor& actor);

struct GameVersion {
    std::string id;
    std::string gameId;
    std::string semver;
    int versionMajor{0};
    int versionMinor{0};
    int versionPatch{0};
    BuildStage stage{BuildStage::Release};
    std::string releaseNotes;
    std::string publishedAt; ///< empty while the version is still a draft
    std::string createdAt;
};

struct NewGameVersion {
    std::string gameId;
    Semver version;
    BuildStage stage{BuildStage::Release};
    std::string releaseNotes;
    bool publish{false};
};

struct Build {
    std::string id;
    std::string gameVersionId;
    BuildPlatform platform{BuildPlatform::Windows};
    BuildArchitecture architecture{BuildArchitecture::X64};
    BuildStatus status{BuildStatus::Uploading};
    std::string manifestSha256;
    int64_t totalSizeBytes{0};
    int32_t fileCount{0};
    std::string entrypointRelativePath;
    std::string defaultLaunchArgs;
    std::string createdAt;
    std::string readyAt;
};

struct NewBuild {
    std::string gameVersionId;
    BuildPlatform platform{BuildPlatform::Windows};
    BuildArchitecture architecture{BuildArchitecture::X64};
};

/// Everything an authorization check needs about a build, resolved in one query so services
/// never walk build → version → game themselves.
struct BuildOwnership {
    std::string buildId;
    std::string gameVersionId;
    std::string gameId;
    std::string publisherUserId;
    GameVisibility visibility{GameVisibility::Draft};
    BuildStatus status{BuildStatus::Uploading};
};

/// May act on a build as its publisher: its owner, or an operator who manages any game.
///
/// Here rather than in a service because both halves of the build lifecycle ask the same
/// question — the upload side to decide who may publish, the download side to decide who may
/// see a draft — and two copies of an authorization rule is one copy too many.
bool mayPublishBuild(const BuildOwnership& ownership, const Actor& actor);

/// May see that a build exists at all. A game still in draft is visible only to its publisher,
/// and to everyone else the build is reported missing rather than forbidden: a 403 would
/// confirm it exists.
bool mayReadBuild(const BuildOwnership& ownership, const Actor& actor);

/// A game together with the versions and builds a particular caller is allowed to see.
struct GameDetail {
    Game game;
    std::vector<GameVersion> versions;
    std::vector<Build> builds;
    std::vector<GameMedia> media;
    bool inLibrary{false};
};

} // namespace launcher::domain
