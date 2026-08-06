#pragma once

#include <string>

#include "services/IMailSender.h"

namespace launcher::services {

/// The two things a message needs that are not about the account it is for.
struct MailContext {
    /// Origin the links in a message are built from, e.g. `https://launcher.example.com`.
    ///
    /// Configuration, never the request's `Host` header: a header is chosen by whoever is
    /// calling, and building the link from it would let a stranger decide which domain appears
    /// in a message delivered to somebody else's inbox.
    std::string linkBaseUrl;
    /// What the deployment calls itself, in the subject line and the signature.
    std::string productName{"Custom Game Launcher"};
};

/// Where the verification link points. Public so the tests can assert on the real URL rather
/// than on a copy of the shape.
std::string verificationLink(const MailContext& context, const std::string& token);

std::string passwordResetLink(const MailContext& context, const std::string& token);

/// The message a new account receives.
///
/// Plain text and English only, deliberately. This server has no localisation of any kind and
/// no idea what language an account reads: nothing stores a locale, and no route sends one. A
/// second translation system living here, for two messages, would be a bigger thing than the
/// feature it serves — so the text is English, and that is written down rather than implied.
MailMessage verificationMessage(const MailContext& context,
                                const std::string& to,
                                const std::string& displayName,
                                const std::string& token);

MailMessage passwordResetMessage(const MailContext& context,
                                 const std::string& to,
                                 const std::string& displayName,
                                 const std::string& token);

} // namespace launcher::services
