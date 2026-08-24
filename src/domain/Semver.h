#pragma once

#include <string>
#include <string_view>

#include "common/Result.h"

namespace launcher::domain {

/// A publisher-set version number.
///
/// The parsed components travel alongside the original text because ordering has to be
/// numeric: compared as text, 0.10.0 sorts before 0.9.0.
struct Semver {
    int major{0};
    int minor{0};
    int patch{0};
    std::string text;
};

/// Largest value a single component may take. Nothing here needs more, and a bound keeps the
/// parse free of overflow handling.
inline constexpr int MAX_VERSION_COMPONENT = 999999;

/// Accepts `1`, `1.2` and `1.2.3`, matching the game_versions CHECK constraint. Omitted
/// components are zero. Leading zeros are rejected so `1.01` and `1.1` cannot both exist as
/// separate versions of the same game.
common::Result<Semver> parseSemver(std::string_view input);

} // namespace launcher::domain
