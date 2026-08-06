#pragma once

#include "services/IMailSender.h"

namespace launcher::services {

/// Writes the message to the log instead of sending it.
///
/// The development transport, and the reason the dev-token fields in the API could be removed
/// rather than replaced: a developer with no relay still gets the link, out of the log, where
/// only somebody who can already read the server's log can see it.
///
/// `AppConfig::validate()` refuses this transport outside development, and that refusal is the
/// whole point — the body of a password-reset message is a live credential, and a deployment
/// that selected this by accident would be writing them into a file with a retention policy.
class LoggingMailSender : public IMailSender {
  public:
    drogon::Task<common::VoidResult> send(MailMessage message) const override;
};

} // namespace launcher::services
