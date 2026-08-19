#include <gtest/gtest.h>

#include <iterator>
#include <set>
#include <string>

#include "common/Error.h"

namespace {

using launcher::common::ApiException;
using launcher::common::Error;
using launcher::common::ErrorCode;

TEST(ErrorTest, MapsEveryCodeToItsHttpStatus) {
    EXPECT_EQ(httpStatusFor(ErrorCode::InvalidInput), 422);
    EXPECT_EQ(httpStatusFor(ErrorCode::Unauthenticated), 401);
    EXPECT_EQ(httpStatusFor(ErrorCode::Forbidden), 403);
    EXPECT_EQ(httpStatusFor(ErrorCode::NotFound), 404);
    EXPECT_EQ(httpStatusFor(ErrorCode::Conflict), 409);
    EXPECT_EQ(httpStatusFor(ErrorCode::QuotaExceeded), 413);
    EXPECT_EQ(httpStatusFor(ErrorCode::RateLimited), 429);
    EXPECT_EQ(httpStatusFor(ErrorCode::DependencyFailure), 503);
    EXPECT_EQ(httpStatusFor(ErrorCode::Internal), 500);
}

TEST(ErrorTest, EveryCodeHasADistinctMachineReadableName) {
    const ErrorCode codes[] = {ErrorCode::InvalidInput,
                               ErrorCode::Unauthenticated,
                               ErrorCode::Forbidden,
                               ErrorCode::NotFound,
                               ErrorCode::Conflict,
                               ErrorCode::QuotaExceeded,
                               ErrorCode::RateLimited,
                               ErrorCode::DependencyFailure,
                               ErrorCode::Internal};

    std::set<std::string> names;
    for (const auto code : codes) {
        names.insert(nameFor(code));
        EXPECT_STRNE(titleFor(code), "");
    }
    EXPECT_EQ(names.size(), std::size(codes));
}

TEST(ErrorTest, ApiExceptionCarriesTheErrorAndItsMessage) {
    const ApiException exception(ErrorCode::Conflict, "slug already taken");

    EXPECT_EQ(exception.error().code, ErrorCode::Conflict);
    EXPECT_EQ(exception.error().detail, "slug already taken");
    EXPECT_STREQ(exception.what(), "slug already taken");
}

TEST(ErrorTest, HelperFactoriesSetTheExpectedCode) {
    EXPECT_EQ(launcher::common::invalidInput("bad").code, ErrorCode::InvalidInput);
    EXPECT_EQ(launcher::common::notFound("gone").code, ErrorCode::NotFound);
    EXPECT_EQ(launcher::common::conflict("dupe").code, ErrorCode::Conflict);
    EXPECT_EQ(launcher::common::internalError("boom").code, ErrorCode::Internal);
}

// A failure that names no rule is the default and stays valid: most of them are not about a
// field somebody typed, and the envelope leaves both keys out rather than sending them empty.
TEST(ErrorTest, AnErrorNamesNoRuleUnlessItIsGivenOne) {
    const Error plain{ErrorCode::InvalidInput, "something is off"};

    EXPECT_TRUE(plain.rule.empty());
    EXPECT_TRUE(plain.ruleArgs.empty());
    EXPECT_TRUE(launcher::common::invalidInput("bad").rule.empty());
}

TEST(ErrorTest, AnErrorCanCarryARuleAndItsArguments) {
    const auto error = launcher::common::invalidInput(
        "password must be at least 8 characters", "password_too_short", {"8"});

    EXPECT_EQ(error.code, ErrorCode::InvalidInput);
    EXPECT_EQ(error.rule, "password_too_short");
    ASSERT_EQ(error.ruleArgs.size(), 1U);
    EXPECT_EQ(error.ruleArgs.front(), "8");
}

TEST(ErrorTest, ARuleSurvivesBeingThrownAndCaught) {
    const ApiException exception(
        launcher::common::invalidInput("slug must not be empty", "slug_required"));

    EXPECT_EQ(exception.error().rule, "slug_required");
}

} // namespace
