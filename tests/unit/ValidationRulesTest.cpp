#include <gtest/gtest.h>

#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <cctype>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <string_view>

#include "app/HttpError.h"
#include "common/Error.h"
#include "domain/Catalog.h"
#include "domain/Media.h"
#include "domain/PatchNote.h"
#include "domain/Semver.h"
#include "domain/ValidationRules.h"

namespace {

namespace rules = launcher::domain::rules;

using launcher::app::makeErrorResponse;
using launcher::common::Error;
using launcher::common::ErrorCode;
using launcher::common::invalidInput;
using launcher::domain::MAX_ALT_TEXT_LENGTH;
using launcher::domain::MAX_PATCH_NOTE_BODY_LENGTH;
using launcher::domain::MAX_PATCH_NOTE_TITLE_LENGTH;
using launcher::domain::MAX_SLUG_LENGTH;
using launcher::domain::MAX_VERSION_COMPONENT;
using launcher::domain::parseSemver;
using launcher::domain::validateAltText;
using launcher::domain::validatePatchNoteBody;
using launcher::domain::validatePatchNoteTitle;
using launcher::domain::validateReleaseDate;
using launcher::domain::validateSlug;

/// Every name in `domain/ValidationRules.h`. Written out rather than generated so that adding
/// a rule without adding it here leaves the uniqueness check below covering less than it
/// claims to, which is the failure mode a generated list would hide.
constexpr const char* EVERY_RULE[] = {
    rules::EMAIL_REQUIRED,
    rules::EMAIL_TOO_LONG,
    rules::EMAIL_INVALID,
    rules::PASSWORD_REQUIRED,
    rules::PASSWORD_TOO_SHORT,
    rules::PASSWORD_TOO_LONG,
    rules::PASSWORD_BLANK,
    rules::DISPLAY_NAME_REQUIRED,
    rules::DISPLAY_NAME_TOO_SHORT,
    rules::DISPLAY_NAME_TOO_LONG,
    rules::DISPLAY_NAME_INVALID,
    rules::ERASURE_REASON_TOO_LONG,
    rules::TITLE_REQUIRED,
    rules::TITLE_TOO_LONG,
    rules::SUMMARY_TOO_LONG,
    rules::DESCRIPTION_TOO_LONG,
    rules::SLUG_REQUIRED,
    rules::SLUG_TOO_LONG,
    rules::SLUG_INVALID,
    rules::RELEASE_DATE_INVALID,
    rules::VERSION_REQUIRED,
    rules::VERSION_INVALID,
    rules::VERSION_TOO_LARGE,
    rules::RELEASE_NOTES_TOO_LONG,
    rules::BUILD_NAME_TOO_LONG,
    rules::PATCH_NOTE_TITLE_REQUIRED,
    rules::PATCH_NOTE_TITLE_TOO_LONG,
    rules::PATCH_NOTE_BODY_TOO_LONG,
    rules::ALT_TEXT_TOO_LONG,
    rules::ALT_TEXT_INVALID,
};

Json::Value bodyOf(const drogon::HttpResponsePtr& response) {
    Json::Value parsed;
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader{builder.newCharReader()};
    const std::string_view body = response->body();
    std::string errors;
    EXPECT_TRUE(reader->parse(body.data(), body.data() + body.size(), &parsed, &errors)) << errors;
    return parsed;
}

} // namespace

// Two clients cannot be told the same thing about two different rules, and a rule that is the
// empty string is one the envelope would silently drop.
TEST(ValidationRulesTest, EveryRuleNameIsDistinctAndNonEmpty) {
    std::set<std::string> names;
    for (const auto* rule : EVERY_RULE) {
        ASSERT_NE(rule, nullptr);
        EXPECT_STRNE(rule, "");
        names.insert(rule);
    }
    EXPECT_EQ(names.size(), std::size(EVERY_RULE));
}

// The spelling is part of the contract: a client keys resource strings off these, and one name
// in a different style is one a translator will get wrong exactly once.
TEST(ValidationRulesTest, EveryRuleNameIsLowercaseSnakeCase) {
    for (const auto* rule : EVERY_RULE) {
        const std::string name{rule};
        for (const char character : name) {
            const auto value = static_cast<unsigned char>(character);
            EXPECT_TRUE(std::islower(value) != 0 || character == '_') << "not snake_case: " << name;
        }
        EXPECT_NE(name.front(), '_') << name;
        EXPECT_NE(name.back(), '_') << name;
    }
}

TEST(ValidationRulesTest, TheEnvelopeCarriesTheRuleAndItsArguments) {
    const auto response = makeErrorResponse(
        invalidInput("password must be at least 8 characters", rules::PASSWORD_TOO_SHORT, {"8"}),
        "req-1");
    const Json::Value body = bodyOf(response);

    EXPECT_EQ(body["code"].asString(), "invalid_input");
    EXPECT_EQ(body["rule"].asString(), "password_too_short");
    ASSERT_TRUE(body["ruleArgs"].isArray());
    ASSERT_EQ(body["ruleArgs"].size(), 1U);
    EXPECT_EQ(body["ruleArgs"][0].asString(), "8");
    // The detail stays what it always was: prose for whoever reads the logs.
    EXPECT_EQ(body["detail"].asString(), "password must be at least 8 characters");
}

// Absent, not empty: a client cannot tell a server too old to send a rule from one that sent
// nothing, and both mean the same thing to it, so neither key is written.
TEST(ValidationRulesTest, TheEnvelopeOmitsBothKeysWhenThereIsNoRule) {
    const Json::Value body =
        bodyOf(makeErrorResponse(Error{ErrorCode::NotFound, "no such game"}, "req-2"));

    EXPECT_FALSE(body.isMember("rule"));
    EXPECT_FALSE(body.isMember("ruleArgs"));
}

TEST(ValidationRulesTest, TheEnvelopeOmitsTheArgumentsOfARuleThatHasNone) {
    const Json::Value body = bodyOf(
        makeErrorResponse(invalidInput("slug must not be empty", rules::SLUG_REQUIRED), "req-3"));

    EXPECT_EQ(body["rule"].asString(), "slug_required");
    EXPECT_FALSE(body.isMember("ruleArgs"));
}

TEST(SlugRuleTest, NamesTheRuleThatRefused) {
    EXPECT_EQ(validateSlug("").error().rule, rules::SLUG_REQUIRED);
    EXPECT_EQ(validateSlug("Not A Slug").error().rule, rules::SLUG_INVALID);
    EXPECT_EQ(validateSlug("trailing-").error().rule, rules::SLUG_INVALID);

    const auto tooLong = validateSlug(std::string(MAX_SLUG_LENGTH + 1, 'a'));
    ASSERT_FALSE(tooLong.ok());
    EXPECT_EQ(tooLong.error().rule, rules::SLUG_TOO_LONG);
    ASSERT_EQ(tooLong.error().ruleArgs.size(), 1U);
    EXPECT_EQ(tooLong.error().ruleArgs.front(), std::to_string(MAX_SLUG_LENGTH));
}

TEST(ReleaseDateRuleTest, NamesTheRuleThatRefused) {
    EXPECT_EQ(validateReleaseDate("30-04-2026").error().rule, rules::RELEASE_DATE_INVALID);
    EXPECT_EQ(validateReleaseDate("2026-02-30").error().rule, rules::RELEASE_DATE_INVALID);
}

TEST(SemverRuleTest, NamesTheRuleThatRefused) {
    EXPECT_EQ(parseSemver("").error().rule, rules::VERSION_REQUIRED);
    EXPECT_EQ(parseSemver("   ").error().rule, rules::VERSION_REQUIRED);
    EXPECT_EQ(parseSemver("1.2.3.4").error().rule, rules::VERSION_INVALID);
    EXPECT_EQ(parseSemver("1..3").error().rule, rules::VERSION_INVALID);
    EXPECT_EQ(parseSemver("1.02.3").error().rule, rules::VERSION_INVALID);
    EXPECT_EQ(parseSemver("1.x.3").error().rule, rules::VERSION_INVALID);

    const auto tooLarge = parseSemver(std::to_string(MAX_VERSION_COMPONENT) + "0.0.0");
    ASSERT_FALSE(tooLarge.ok());
    EXPECT_EQ(tooLarge.error().rule, rules::VERSION_TOO_LARGE);
    ASSERT_EQ(tooLarge.error().ruleArgs.size(), 1U);
    EXPECT_EQ(tooLarge.error().ruleArgs.front(), std::to_string(MAX_VERSION_COMPONENT));
}

TEST(PatchNoteRuleTest, NamesTheRuleThatRefused) {
    EXPECT_EQ(validatePatchNoteTitle("   ").error().rule, rules::PATCH_NOTE_TITLE_REQUIRED);

    const auto title = validatePatchNoteTitle(std::string(MAX_PATCH_NOTE_TITLE_LENGTH + 1, 'a'));
    ASSERT_FALSE(title.ok());
    EXPECT_EQ(title.error().rule, rules::PATCH_NOTE_TITLE_TOO_LONG);
    ASSERT_EQ(title.error().ruleArgs.size(), 1U);
    EXPECT_EQ(title.error().ruleArgs.front(), std::to_string(MAX_PATCH_NOTE_TITLE_LENGTH));

    const auto body = validatePatchNoteBody(std::string(MAX_PATCH_NOTE_BODY_LENGTH + 1, 'a'));
    ASSERT_FALSE(body.ok());
    EXPECT_EQ(body.error().rule, rules::PATCH_NOTE_BODY_TOO_LONG);
    ASSERT_EQ(body.error().ruleArgs.size(), 1U);
    EXPECT_EQ(body.error().ruleArgs.front(), std::to_string(MAX_PATCH_NOTE_BODY_LENGTH));
}

TEST(AltTextRuleTest, NamesTheRuleThatRefused) {
    const auto tooLong = validateAltText(std::string(MAX_ALT_TEXT_LENGTH + 1, 'a'));
    ASSERT_FALSE(tooLong.ok());
    EXPECT_EQ(tooLong.error().rule, rules::ALT_TEXT_TOO_LONG);
    ASSERT_EQ(tooLong.error().ruleArgs.size(), 1U);
    EXPECT_EQ(tooLong.error().ruleArgs.front(), std::to_string(MAX_ALT_TEXT_LENGTH));

    // Built from its code point rather than typed: a control character in a source file is
    // invisible in a diff and in an editor (see the CLAUDE.md gotcha table).
    EXPECT_EQ(validateAltText(std::string("a") + static_cast<char>(0x01)).error().rule,
              rules::ALT_TEXT_INVALID);
}
