#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "services/IMailSender.h"

namespace launcher::testing {

/// Keeps every message instead of sending it, and can be told to refuse.
///
/// This is what makes the flows testable now that no token is returned in a response: the
/// integration harness installs one and reads the link out of the message, exactly where a
/// person would read it. The refusal switch is the more important half — a registration whose
/// message does not go out is a *behaviour*, not an interface, and this is what lets it be
/// asserted on.
///
/// Locked because the integration harness reaches it from the server's event-loop threads.
class FakeMailSender : public services::IMailSender {
  public:
    drogon::Task<common::VoidResult> send(services::MailMessage message) const override {
        {
            const std::lock_guard<std::mutex> guard(mutex_);
            if (failing_) {
                ++refused_;
                co_return common::VoidResult::failure(common::ErrorCode::DependencyFailure,
                                                      "the fake mail sender was told to refuse");
            }
            sent_.push_back(std::move(message));
        }
        co_return common::VoidResult::success();
    }

    /// Every later send fails until this is turned back off.
    void refuseEverything(bool refusing) {
        const std::lock_guard<std::mutex> guard(mutex_);
        failing_ = refusing;
    }

    std::size_t sentCount() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return sent_.size();
    }

    std::size_t refusedCount() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return refused_;
    }

    void clear() {
        const std::lock_guard<std::mutex> guard(mutex_);
        sent_.clear();
        refused_ = 0;
        failing_ = false;
    }

    std::optional<services::MailMessage> lastTo(const std::string& address) const {
        const std::lock_guard<std::mutex> guard(mutex_);
        for (auto message = sent_.rbegin(); message != sent_.rend(); ++message) {
            if (message->to == address) {
                return *message;
            }
        }
        return std::nullopt;
    }

    /// The `token=` value out of the link in the last message sent to that address.
    ///
    /// Parsed from the body rather than handed over separately, so a test only ever sees what
    /// the recipient sees: a message whose link is malformed fails a test instead of passing
    /// one through a side channel.
    std::string tokenIn(const std::string& address) const {
        const auto message = lastTo(address);
        if (!message.has_value()) {
            return {};
        }

        const auto marker = message->body.find("token=");
        if (marker == std::string::npos) {
            return {};
        }

        const auto start = marker + std::string("token=").size();
        const auto end = message->body.find_first_of(" \r\n", start);
        return message->body.substr(start, end == std::string::npos ? end : end - start);
    }

  private:
    mutable std::mutex mutex_;
    mutable std::vector<services::MailMessage> sent_;
    mutable std::size_t refused_{0};
    bool failing_{false};
};

} // namespace launcher::testing
