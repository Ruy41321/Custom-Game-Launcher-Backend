#include <gtest/gtest.h>

#include <string>

#include "services/MailTemplates.h"

namespace {

using launcher::services::MailContext;
using launcher::services::passwordResetLink;
using launcher::services::passwordResetMessage;
using launcher::services::verificationLink;
using launcher::services::verificationMessage;

MailContext context(const std::string& baseUrl = "https://launcher.example.com") {
    MailContext value;
    value.linkBaseUrl = baseUrl;
    value.productName = "Test Launcher";
    return value;
}

TEST(MailTemplatesTest, BuildsLinksAgainstTheConfiguredOrigin) {
    EXPECT_EQ(verificationLink(context(), "abc123"),
              "https://launcher.example.com/verify-email?token=abc123");
    EXPECT_EQ(passwordResetLink(context(), "abc123"),
              "https://launcher.example.com/password-reset?token=abc123");
}

// A base URL is typed by a person into an environment variable, and half of them end in a
// slash. Doubling it would produce a link that 404s on a deployment that looked correct.
TEST(MailTemplatesTest, DoesNotDoubleTheSlashOfATrailingBaseUrl) {
    EXPECT_EQ(verificationLink(context("https://launcher.example.com/"), "abc123"),
              "https://launcher.example.com/verify-email?token=abc123");
}

TEST(MailTemplatesTest, TheVerificationMessageCarriesTheLinkAndTheProductName) {
    const auto message = verificationMessage(context(), "dev@example.com", "Dev", "abc123");

    EXPECT_EQ(message.to, "dev@example.com");
    EXPECT_NE(message.subject.find("Test Launcher"), std::string::npos);
    EXPECT_NE(message.body.find("Hello Dev,"), std::string::npos);
    EXPECT_NE(message.body.find(verificationLink(context(), "abc123")), std::string::npos);
}

TEST(MailTemplatesTest, TheResetMessageSaysNothingChangesUntilTheLinkIsOpened) {
    const auto message = passwordResetMessage(context(), "dev@example.com", "Dev", "abc123");

    EXPECT_NE(message.body.find(passwordResetLink(context(), "abc123")), std::string::npos);
    EXPECT_NE(message.body.find("does not change"), std::string::npos)
        << "somebody who did not ask for this has to be told they need do nothing";
}

// Nothing that reaches a message from an account may bring its own line breaks with it.
TEST(MailTemplatesTest, FoldsADisplayNameOntoOneLine) {
    const std::string hostile = std::string("Dev\r\nBcc: victim@example.com");

    const auto message = verificationMessage(context(), "dev@example.com", hostile, "abc123");

    EXPECT_EQ(message.body.find("\r\n"), std::string::npos);
    EXPECT_NE(message.body.find("Dev  Bcc: victim@example.com"), std::string::npos);
}

TEST(MailTemplatesTest, GreetsWithoutANameWhenThereIsNone) {
    const auto message = verificationMessage(context(), "dev@example.com", "", "abc123");

    EXPECT_NE(message.body.find("Hello,"), std::string::npos);
}

} // namespace
