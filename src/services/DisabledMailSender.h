#pragma once

#include "services/IMailSender.h"

namespace launcher::services {

/// What a deployment that turned mail off installs.
///
/// Nothing should reach it: the routes that send are answered with a 404 by
/// `MailRateLimitFilter`, and `AuthService` does not compose a verification message when
/// delivery is off. It exists so that a *fourth* path added later, which forgot both of those,
/// fails and says so rather than appearing to have sent something.
class DisabledMailSender : public IMailSender {
  public:
    drogon::Task<common::VoidResult> send(MailMessage) const override {
        co_return common::VoidResult::failure(common::ErrorCode::DependencyFailure,
                                              "mail delivery is turned off for this deployment");
    }
};

} // namespace launcher::services
