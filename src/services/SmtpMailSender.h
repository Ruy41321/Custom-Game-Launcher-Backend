#pragma once

#include <trantor/net/EventLoopThread.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "services/IMailSender.h"

namespace launcher::services {

/// How the connection to the relay is protected.
enum class SmtpSecurity {
    /// Plain SMTP. Only ever correct for a relay on the same host or the same private network
    /// — a local Postfix, or the development mail catcher.
    None,
    /// Connect in the clear and refuse to continue unless STARTTLS succeeds. The submission
    /// port's normal shape, and the default.
    StartTls,
    /// TLS from the first byte, the implicit form on port 465.
    Tls,
};

struct SmtpSettings {
    std::string host;
    uint16_t port{587};
    std::string username;
    std::string password;
    SmtpSecurity security{SmtpSecurity::StartTls};
    std::string fromAddress;
    std::string fromName;
    /// Bounds the whole conversation, not just the connect. A registration waits for this
    /// before it can tell the caller whether the message went out, so it is short on purpose.
    std::chrono::seconds timeout{10};
};

/// Delivery over SMTP, through libcurl.
///
/// libcurl rather than a client written here, for one reason that outweighs the others: it
/// verifies the relay's certificate, and has done correctly for twenty years. A hand-written
/// SMTP client over trantor would be three hundred lines of asynchronous state machine that
/// *this suite cannot test* — there is no mail server in it — with certificate validation
/// among the things we would have to get right ourselves.
///
/// The conversation is blocking, so it runs on a thread of its own and the coroutine resumes
/// on the loop it was suspended from. One thread, so messages leave one at a time: a
/// deployment of this size sends a message per registration, and serialising them keeps a slow
/// relay from occupying anything but this thread.
class SmtpMailSender : public IMailSender {
  public:
    explicit SmtpMailSender(SmtpSettings settings);
    ~SmtpMailSender() override;

    SmtpMailSender(const SmtpMailSender&) = delete;
    SmtpMailSender& operator=(const SmtpMailSender&) = delete;

    drogon::Task<common::VoidResult> send(MailMessage message) const override;

  private:
    /// The blocking half, run on `worker_`.
    common::VoidResult deliver(const MailMessage& message) const;

    SmtpSettings settings_;
    std::unique_ptr<trantor::EventLoopThread> worker_;
};

} // namespace launcher::services
