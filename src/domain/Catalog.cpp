#include "domain/Catalog.h"

#include <array>
#include <cctype>

namespace launcher::domain {
namespace {

using common::ErrorCode;
using common::VoidResult;

bool isDigits(std::string_view text) {
    for (const char character : text) {
        if (std::isdigit(static_cast<unsigned char>(character)) == 0) {
            return false;
        }
    }
    return !text.empty();
}

int daysInMonth(int year, int month) {
    static constexpr std::array<int, 12> LENGTHS{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        return leap ? 29 : 28;
    }
    return LENGTHS[static_cast<std::size_t>(month - 1)];
}

} // namespace

const char* toString(GameVisibility value) {
    switch (value) {
    case GameVisibility::Unlisted:
        return "unlisted";
    case GameVisibility::Public:
        return "public";
    case GameVisibility::Draft:
        break;
    }
    return "draft";
}

const char* toString(BuildStage value) {
    switch (value) {
    case BuildStage::Demo:
        return "demo";
    case BuildStage::Alpha:
        return "alpha";
    case BuildStage::Beta:
        return "beta";
    case BuildStage::Release:
        break;
    }
    return "release";
}

const char* toString(BuildPlatform value) {
    switch (value) {
    case BuildPlatform::Linux:
        return "linux";
    case BuildPlatform::MacOS:
        return "macos";
    case BuildPlatform::Windows:
        break;
    }
    return "windows";
}

const char* toString(BuildArchitecture value) {
    switch (value) {
    case BuildArchitecture::Arm64:
        return "arm64";
    case BuildArchitecture::X64:
        break;
    }
    return "x64";
}

const char* toString(BuildStatus value) {
    switch (value) {
    case BuildStatus::Ready:
        return "ready";
    case BuildStatus::Failed:
        return "failed";
    case BuildStatus::Uploading:
        break;
    }
    return "uploading";
}

std::optional<GameVisibility> parseGameVisibility(std::string_view value) {
    if (value == "draft") {
        return GameVisibility::Draft;
    }
    if (value == "unlisted") {
        return GameVisibility::Unlisted;
    }
    if (value == "public") {
        return GameVisibility::Public;
    }
    return std::nullopt;
}

std::optional<BuildStage> parseBuildStage(std::string_view value) {
    if (value == "demo") {
        return BuildStage::Demo;
    }
    if (value == "alpha") {
        return BuildStage::Alpha;
    }
    if (value == "beta") {
        return BuildStage::Beta;
    }
    if (value == "release") {
        return BuildStage::Release;
    }
    return std::nullopt;
}

std::optional<BuildPlatform> parseBuildPlatform(std::string_view value) {
    if (value == "windows") {
        return BuildPlatform::Windows;
    }
    if (value == "linux") {
        return BuildPlatform::Linux;
    }
    if (value == "macos") {
        return BuildPlatform::MacOS;
    }
    return std::nullopt;
}

std::optional<BuildArchitecture> parseBuildArchitecture(std::string_view value) {
    if (value == "x64") {
        return BuildArchitecture::X64;
    }
    if (value == "arm64") {
        return BuildArchitecture::Arm64;
    }
    return std::nullopt;
}

std::optional<BuildStatus> parseBuildStatus(std::string_view value) {
    if (value == "uploading") {
        return BuildStatus::Uploading;
    }
    if (value == "ready") {
        return BuildStatus::Ready;
    }
    if (value == "failed") {
        return BuildStatus::Failed;
    }
    return std::nullopt;
}

VoidResult validateSlug(std::string_view slug) {
    if (slug.empty()) {
        return VoidResult::failure(ErrorCode::InvalidInput, "slug must not be empty");
    }
    if (slug.size() > MAX_SLUG_LENGTH) {
        return VoidResult::failure(ErrorCode::InvalidInput,
                                   "slug must be at most " + std::to_string(MAX_SLUG_LENGTH) +
                                       " characters");
    }

    bool previousWasHyphen = true; // a leading hyphen is as invalid as a doubled one
    for (const char character : slug) {
        const bool alphanumeric =
            (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
        if (alphanumeric) {
            previousWasHyphen = false;
            continue;
        }
        if (character == '-' && !previousWasHyphen) {
            previousWasHyphen = true;
            continue;
        }
        return VoidResult::failure(
            ErrorCode::InvalidInput,
            "slug must be lowercase letters, digits and single hyphens, e.g. my-great-game");
    }

    if (previousWasHyphen) {
        return VoidResult::failure(ErrorCode::InvalidInput, "slug must not end with a hyphen");
    }
    return VoidResult::success();
}

std::string slugify(std::string_view title) {
    std::string slug;
    slug.reserve(title.size());

    for (const char character : title) {
        const auto raw = static_cast<unsigned char>(character);
        if (std::isalnum(raw) != 0) {
            slug.push_back(static_cast<char>(std::tolower(raw)));
        } else if (!slug.empty() && slug.back() != '-') {
            slug.push_back('-');
        }
    }

    while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    if (slug.size() > MAX_SLUG_LENGTH) {
        slug.resize(MAX_SLUG_LENGTH);
        while (!slug.empty() && slug.back() == '-') {
            slug.pop_back();
        }
    }
    return slug;
}

VoidResult validateReleaseDate(std::string_view date) {
    constexpr std::size_t ISO_DATE_LENGTH = 10;
    const auto malformed = VoidResult::failure(ErrorCode::InvalidInput,
                                               "releaseDate must be an ISO date, e.g. 2026-04-30");

    if (date.size() != ISO_DATE_LENGTH || date[4] != '-' || date[7] != '-') {
        return malformed;
    }
    if (!isDigits(date.substr(0, 4)) || !isDigits(date.substr(5, 2)) ||
        !isDigits(date.substr(8, 2))) {
        return malformed;
    }

    const int year = std::stoi(std::string(date.substr(0, 4)));
    const int month = std::stoi(std::string(date.substr(5, 2)));
    const int day = std::stoi(std::string(date.substr(8, 2)));

    if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        day > daysInMonth(year, month)) {
        return malformed;
    }
    return VoidResult::success();
}

bool mayEditGame(const Game& game, const Actor& actor) {
    return actor.owns(game.publisherUserId) || actor.managesAnyGame();
}

bool mayViewGame(const Game& game, const Actor& actor) {
    return game.visibility != GameVisibility::Draft || mayEditGame(game, actor);
}

bool mayPublishBuild(const BuildOwnership& ownership, const Actor& actor) {
    return actor.owns(ownership.publisherUserId) || actor.managesAnyGame();
}

bool mayReadBuild(const BuildOwnership& ownership, const Actor& actor) {
    return ownership.visibility != GameVisibility::Draft || mayPublishBuild(ownership, actor);
}

} // namespace launcher::domain
