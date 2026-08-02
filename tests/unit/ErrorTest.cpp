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

} // namespace
