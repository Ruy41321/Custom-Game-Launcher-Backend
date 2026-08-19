#include <gtest/gtest.h>

#include <string>

#include "domain/Validation.h"
#include "domain/ValidationRules.h"

namespace {

namespace rules = launcher::domain::rules;

using launcher::common::ErrorCode;
using launcher::domain::MAX_DISPLAY_NAME_LENGTH;
using launcher::domain::MAX_EMAIL_LENGTH;
using launcher::domain::MAX_PASSWORD_LENGTH;
using launcher::domain::MIN_DISPLAY_NAME_LENGTH;
using launcher::domain::MIN_PASSWORD_LENGTH;
using launcher::domain::normalizeEmail;
using launcher::domain::trim;
using launcher::domain::validateDisplayName;
using launcher::domain::validateEmail;
using launcher::domain::validatePassword;

TEST(TrimTest, RemovesSurroundingWhitespaceOnly) {
    EXPECT_EQ(trim("  hello  "), "hello");
    EXPECT_EQ(trim("\t\nhello\r\n"), "hello");
    EXPECT_EQ(trim("hello world"), "hello world");
    EXPECT_EQ(trim("   "), "");
    EXPECT_EQ(trim(""), "");
}

TEST(NormalizeEmailTest, TrimsAndLowercases) {
    EXPECT_EQ(normalizeEmail("  Dev@Example.COM "), "dev@example.com");
    EXPECT_EQ(normalizeEmail("already@lower.dev"), "already@lower.dev");
}

TEST(ValidateEmailTest, AcceptsOrdinaryAddresses) {
    for (const auto* email : {"dev@example.com",
                              "first.last@example.co.uk",
                              "user+tag@example.dev",
                              "u@a.io",
                              "UPPER@EXAMPLE.COM"}) {
        EXPECT_TRUE(validateEmail(email).ok()) << "rejected: " << email;
    }
}

TEST(ValidateEmailTest, RejectsStructurallyBrokenAddresses) {
    for (const auto* email : {"",
                              "   ",
                              "no-at-sign.example.com",
                              "@example.com",
                              "user@",
                              "user@@example.com",
                              "user@example",
                              "user@.com",
                              "user@example.",
                              "user name@example.com"}) {
        EXPECT_FALSE(validateEmail(email).ok()) << "accepted: " << email;
    }
}

TEST(ValidateEmailTest, RejectsAnOverlongAddress) {
    const std::string local(250, 'a');

    EXPECT_FALSE(validateEmail(local + "@example.com").ok());
}

TEST(ValidateEmailTest, RejectsControlCharacters) {
    EXPECT_FALSE(validateEmail(std::string("dev\n@example.com")).ok());
    EXPECT_FALSE(validateEmail(std::string("dev\0@example.com", 16)).ok());
}

TEST(ValidateEmailTest, ReportsInvalidInput) {
    const auto result = validateEmail("nope");

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
}

TEST(ValidatePasswordTest, AcceptsALongPassphrase) {
    EXPECT_TRUE(validatePassword("correct horse battery staple").ok());
}

// Length is the rule; composition is not. A long lowercase passphrase must be accepted.
TEST(ValidatePasswordTest, DoesNotRequireCharacterClasses) {
    EXPECT_TRUE(validatePassword("aaaaaaaaaaaaaaaa").ok());
}

TEST(ValidatePasswordTest, RejectsAnythingShorterThanTheMinimum) {
    EXPECT_FALSE(validatePassword(std::string(MIN_PASSWORD_LENGTH - 1, 'x')).ok());
    EXPECT_TRUE(validatePassword(std::string(MIN_PASSWORD_LENGTH, 'x')).ok());
}

// An unbounded password would let anyone turn a login form into an Argon2id workload.
TEST(ValidatePasswordTest, RejectsAnOverlongPassword) {
    EXPECT_TRUE(validatePassword(std::string(MAX_PASSWORD_LENGTH, 'x')).ok());
    EXPECT_FALSE(validatePassword(std::string(MAX_PASSWORD_LENGTH + 1, 'x')).ok());
}

TEST(ValidatePasswordTest, RejectsWhitespaceOnly) {
    EXPECT_FALSE(validatePassword("                    ").ok());
}

TEST(ValidateDisplayNameTest, AcceptsOrdinaryNames) {
    EXPECT_TRUE(validateDisplayName("Luigi").ok());
    EXPECT_TRUE(validateDisplayName("Ada L.").ok());
    EXPECT_TRUE(validateDisplayName("  padded  ").ok());
}

TEST(ValidateDisplayNameTest, RejectsTooShortTooLongAndControlCharacters) {
    EXPECT_FALSE(validateDisplayName("a").ok());
    EXPECT_FALSE(validateDisplayName("").ok());
    EXPECT_FALSE(validateDisplayName(std::string(65, 'a')).ok());
    EXPECT_FALSE(validateDisplayName("bad\nname").ok());
}

// The rule names below are the contract a client translates. `detail` is free to be reworded;
// these are not, which is the whole reason they exist — see domain/ValidationRules.h.

TEST(AccountRuleTest, EmailNamesTheRuleThatRefused) {
    EXPECT_EQ(validateEmail("").error().rule, rules::EMAIL_REQUIRED);
    EXPECT_EQ(validateEmail("   ").error().rule, rules::EMAIL_REQUIRED);

    const auto tooLong = validateEmail(std::string(250, 'a') + "@example.com");
    ASSERT_FALSE(tooLong.ok());
    EXPECT_EQ(tooLong.error().rule, rules::EMAIL_TOO_LONG);
    ASSERT_EQ(tooLong.error().ruleArgs.size(), 1U);
    EXPECT_EQ(tooLong.error().ruleArgs.front(), std::to_string(MAX_EMAIL_LENGTH));
}

// Six ways of writing an address wrong are one rule, because the person typing has one thing
// to do about all six. The detail still says which.
TEST(AccountRuleTest, EveryMalformedAddressIsOneRule) {
    for (const auto* email : {"no-at-sign.example.com",
                              "@example.com",
                              "user@",
                              "user@@example.com",
                              "user@example",
                              "user@.com",
                              "user@example.",
                              "user name@example.com"}) {
        const auto result = validateEmail(email);
        ASSERT_FALSE(result.ok()) << "accepted: " << email;
        EXPECT_EQ(result.error().rule, rules::EMAIL_INVALID) << email;
        EXPECT_FALSE(result.error().detail.empty()) << email;
    }
}

TEST(AccountRuleTest, PasswordNamesTheRuleAndTheLimitItBroke) {
    const auto tooShort = validatePassword(std::string(MIN_PASSWORD_LENGTH - 1, 'x'));
    ASSERT_FALSE(tooShort.ok());
    EXPECT_EQ(tooShort.error().rule, rules::PASSWORD_TOO_SHORT);
    ASSERT_EQ(tooShort.error().ruleArgs.size(), 1U);
    EXPECT_EQ(tooShort.error().ruleArgs.front(), std::to_string(MIN_PASSWORD_LENGTH));

    const auto tooLong = validatePassword(std::string(MAX_PASSWORD_LENGTH + 1, 'x'));
    ASSERT_FALSE(tooLong.ok());
    EXPECT_EQ(tooLong.error().rule, rules::PASSWORD_TOO_LONG);
    ASSERT_EQ(tooLong.error().ruleArgs.size(), 1U);
    EXPECT_EQ(tooLong.error().ruleArgs.front(), std::to_string(MAX_PASSWORD_LENGTH));

    EXPECT_EQ(validatePassword("                    ").error().rule, rules::PASSWORD_BLANK);
}

TEST(AccountRuleTest, DisplayNameNamesTheRuleAndTheLimitItBroke) {
    const auto tooShort = validateDisplayName("a");
    ASSERT_FALSE(tooShort.ok());
    EXPECT_EQ(tooShort.error().rule, rules::DISPLAY_NAME_TOO_SHORT);
    ASSERT_EQ(tooShort.error().ruleArgs.size(), 1U);
    EXPECT_EQ(tooShort.error().ruleArgs.front(), std::to_string(MIN_DISPLAY_NAME_LENGTH));

    const auto tooLong = validateDisplayName(std::string(MAX_DISPLAY_NAME_LENGTH + 1, 'a'));
    ASSERT_FALSE(tooLong.ok());
    EXPECT_EQ(tooLong.error().rule, rules::DISPLAY_NAME_TOO_LONG);
    ASSERT_EQ(tooLong.error().ruleArgs.size(), 1U);
    EXPECT_EQ(tooLong.error().ruleArgs.front(), std::to_string(MAX_DISPLAY_NAME_LENGTH));

    EXPECT_EQ(validateDisplayName("bad\nname").error().rule, rules::DISPLAY_NAME_INVALID);
}

} // namespace
