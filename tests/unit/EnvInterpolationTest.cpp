#include <gtest/gtest.h>

#include <map>
#include <string>

#include "common/EnvInterpolation.h"

namespace {

using launcher::common::EnvLookup;
using launcher::common::ErrorCode;
using launcher::common::interpolateEnv;

EnvLookup lookupFrom(std::map<std::string, std::string> values) {
    return [values = std::move(values)](const std::string& name) -> std::optional<std::string> {
        const auto found = values.find(name);
        if (found == values.end()) {
            return std::nullopt;
        }
        return found->second;
    };
}

TEST(EnvInterpolationTest, LeavesTextWithoutPlaceholdersUntouched) {
    const auto result = interpolateEnv(R"({"host":"localhost"})", lookupFrom({}));

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), R"({"host":"localhost"})");
}

TEST(EnvInterpolationTest, SubstitutesASetVariable) {
    const auto result = interpolateEnv("host=${DB_HOST}", lookupFrom({{"DB_HOST", "db"}}));

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), "host=db");
}

TEST(EnvInterpolationTest, SubstitutesSeveralOccurrencesIncludingRepeats) {
    const auto result = interpolateEnv("${A}-${B}-${A}", lookupFrom({{"A", "one"}, {"B", "two"}}));

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), "one-two-one");
}

TEST(EnvInterpolationTest, UsesTheDefaultWhenTheVariableIsUnset) {
    const auto result = interpolateEnv("port=${DB_PORT:-5432}", lookupFrom({}));

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), "port=5432");
}

TEST(EnvInterpolationTest, PrefersTheEnvironmentOverTheDefault) {
    const auto result = interpolateEnv("port=${DB_PORT:-5432}", lookupFrom({{"DB_PORT", "6543"}}));

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), "port=6543");
}

TEST(EnvInterpolationTest, AnEmptyDefaultIsAllowedAndYieldsAnEmptyString) {
    const auto result = interpolateEnv("dir=[${LOG_DIR:-}]", lookupFrom({}));

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), "dir=[]");
}

TEST(EnvInterpolationTest, AnEmptyEnvironmentValueIsUsedAsIs) {
    const auto result = interpolateEnv("dir=[${LOG_DIR:-fallback}]", lookupFrom({{"LOG_DIR", ""}}));

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), "dir=[]");
}

// A blank JWT secret or database password must never be produced silently — that is the
// whole reason a missing variable is an error instead of an empty expansion.
TEST(EnvInterpolationTest, AMissingVariableWithoutADefaultIsAnError) {
    const auto result = interpolateEnv(R"("jwtSecret":"${JWT_SECRET}")", lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(result.error().detail.find("JWT_SECRET"), std::string::npos);
}

TEST(EnvInterpolationTest, AnUnterminatedPlaceholderIsAnError) {
    const auto result = interpolateEnv("value=${BROKEN", lookupFrom({{"BROKEN", "x"}}));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
}

TEST(EnvInterpolationTest, AnEmptyVariableNameIsAnError) {
    const auto result = interpolateEnv("value=${}", lookupFrom({}));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
}

TEST(EnvInterpolationTest, ALoneDollarSignIsNotAPlaceholder) {
    const auto result = interpolateEnv("cost is $5 or ${A}", lookupFrom({{"A", "free"}}));

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), "cost is $5 or free");
}

TEST(EnvInterpolationTest, DefaultsMayContainColonsAndSlashes) {
    const auto result = interpolateEnv("${URL:-http://localhost:8081/files}", lookupFrom({}));

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), "http://localhost:8081/files");
}

} // namespace
