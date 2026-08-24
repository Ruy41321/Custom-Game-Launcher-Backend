#include "services/LoggingMailSender.h"

#include <spdlog/spdlog.h>

#include "common/Logging.h"

namespace launcher::services {

drogon::Task<common::VoidResult> LoggingMailSender::send(MailMessage message) const {
    spdlog::info("mail transport is 'log'; not sending to={} subject={} body={}",
                 common::escapeJson(message.to),
                 common::escapeJson(message.subject),
                 common::escapeJson(message.body));
    co_return common::VoidResult::success();
}

} // namespace launcher::services
