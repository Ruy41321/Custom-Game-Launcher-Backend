#include "services/MailTemplates.h"

namespace launcher::services {
namespace {

/// Everything that reaches a message from an account is folded onto one line first.
///
/// A display name is validated on the way in and a mail body is not a header, so neither half
/// of an injection is available today; this is here because it costs nothing and does not
/// depend on a rule that lives in another file staying what it is.
std::string singleLine(const std::string& value) {
    std::string flattened;
    flattened.reserve(value.size());
    for (const char character : value) {
        flattened.push_back(character == '\r' || character == '\n' ? ' ' : character);
    }
    return flattened;
}

std::string withoutTrailingSlash(const std::string& url) {
    if (!url.empty() && url.back() == '/') {
        return url.substr(0, url.size() - 1);
    }
    return url;
}

/// A greeting that reads as one whether or not there is a name to use.
std::string greeting(const std::string& displayName) {
    const auto name = singleLine(displayName);
    return name.empty() ? "Hello," : "Hello " + name + ",";
}

} // namespace

std::string verificationLink(const MailContext& context, const std::string& token) {
    // The token is base64url, so it needs no percent-encoding to survive a query string.
    return withoutTrailingSlash(context.linkBaseUrl) + "/verify-email?token=" + token;
}

std::string passwordResetLink(const MailContext& context, const std::string& token) {
    return withoutTrailingSlash(context.linkBaseUrl) + "/password-reset?token=" + token;
}

MailMessage verificationMessage(const MailContext& context,
                                const std::string& to,
                                const std::string& displayName,
                                const std::string& token) {
    MailMessage message;
    message.to = to;
    message.subject = "Confirm your " + context.productName + " address";
    message.body = greeting(displayName) + "\n\n" + "Open this link to confirm this address " +
                   "and finish creating your " + context.productName + " account:\n\n" +
                   verificationLink(context, token) + "\n\n" +
                   "If you did not create an account, ignore this message: the address stays "
                   "unconfirmed and nothing else happens.\n\n" +
                   "-- \n" + context.productName + "\n";
    return message;
}

MailMessage passwordResetMessage(const MailContext& context,
                                 const std::string& to,
                                 const std::string& displayName,
                                 const std::string& token) {
    MailMessage message;
    message.to = to;
    message.subject = "Reset your " + context.productName + " password";
    message.body = greeting(displayName) + "\n\n" + "Open this link to choose a new password:\n\n" +
                   passwordResetLink(context, token) + "\n\n" +
                   "The link can be used once, and requesting another one replaces it. If you "
                   "did not ask for this, ignore this message: your password does not change "
                   "until somebody opens the link.\n\n" +
                   "-- \n" + context.productName + "\n";
    return message;
}

} // namespace launcher::services
