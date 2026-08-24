#pragma once

#include <drogon/utils/coroutine.h>

#include <string>

#include "common/Result.h"

namespace launcher::services {

/// One outbound message: plain text, UTF-8, one recipient.
///
/// There is no HTML part and no attachment, because nothing this server sends needs one. A
/// verification link and a reset link are a sentence and a URL, and a text/plain message
/// renders in every client there is, cannot carry a tracking pixel, and cannot be rendered
/// differently from what it says.
struct MailMessage {
    std::string to;
    std::string subject;
    std::string body;
};

/// Delivery, as seen by whoever decides that something should be sent.
///
/// The interface lives beside the services rather than in `storage/` because the caller is
/// `AuthService`: it decides *when* an address needs confirming, and knows nothing about SMTP,
/// TLS or retries. That split is what lets every rule around sending — a registration that
/// survives a failed send, a reset that reports the same thing either way — be unit tested
/// with no socket in sight.
class IMailSender {
  public:
    virtual ~IMailSender() = default;

    /// Delivers the message, or explains why it could not.
    ///
    /// A failure is a `Result` and never an exception: not sending an email is an expected
    /// outcome that the caller has a policy for, not an exceptional one. Taken by value
    /// because this is a coroutine parameter — a reference would dangle at the first
    /// suspension (D13).
    virtual drogon::Task<common::VoidResult> send(MailMessage message) const = 0;
};

} // namespace launcher::services
