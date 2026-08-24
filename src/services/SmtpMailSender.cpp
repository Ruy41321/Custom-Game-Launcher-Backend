#include "services/SmtpMailSender.h"

#include <curl/curl.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <mutex>
#include <utility>
#include <vector>

#include "common/Random.h"

namespace launcher::services {
namespace {

using common::VoidResult;

/// curl_global_init is not thread safe and has to happen before the first easy handle.
void initCurlOnce() {
    static std::once_flag once;
    std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

bool isAscii(const std::string& value) {
    return std::none_of(value.begin(), value.end(), [](char character) {
        return static_cast<unsigned char>(character) >= 0x80;
    });
}

/// RFC 2047 encoded word, for a subject that is not plain ASCII.
///
/// The subject carries the deployment's own product name, which a fork is free to write in its
/// own language; a raw UTF-8 header would be delivered by most relays and mangled by some.
std::string encodedWord(const std::string& value) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    const std::size_t encodedSize =
        sodium_base64_encoded_len(value.size(), sodium_base64_VARIANT_ORIGINAL);
    std::string encoded(encodedSize, '\0');
    sodium_bin2base64(
        encoded.data(), encoded.size(), bytes, value.size(), sodium_base64_VARIANT_ORIGINAL);
    encoded.resize(std::strlen(encoded.c_str()));
    return "=?UTF-8?B?" + encoded + "?=";
}

std::string headerSafe(const std::string& value) {
    std::string flattened;
    flattened.reserve(value.size());
    for (const char character : value) {
        if (character != '\r' && character != '\n') {
            flattened.push_back(character);
        }
    }
    return isAscii(flattened) ? flattened : encodedWord(flattened);
}

/// `Date:` in the only format RFC 5322 defines, in UTC so no host's timezone data is involved.
std::string rfc5322Date() {
    const auto now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif

    // Deliberately not strftime with %a/%b: those are locale-dependent, and this header is not.
    static constexpr std::array<const char*, 7> DAYS{
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static constexpr std::array<const char*, 12> MONTHS{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    std::array<char, 64> buffer{};
    std::snprintf(buffer.data(),
                  buffer.size(),
                  "%s, %02d %s %04d %02d:%02d:%02d +0000",
                  DAYS.at(static_cast<std::size_t>(utc.tm_wday) % DAYS.size()),
                  utc.tm_mday,
                  MONTHS.at(static_cast<std::size_t>(utc.tm_mon) % MONTHS.size()),
                  utc.tm_year + 1900,
                  utc.tm_hour,
                  utc.tm_min,
                  utc.tm_sec);
    return buffer.data();
}

std::string domainOf(const std::string& address) {
    const auto at = address.rfind('@');
    return at == std::string::npos ? "localhost" : address.substr(at + 1);
}

/// Bare newlines become CRLF, which is what SMTP transports and what curl expects to be handed.
std::string withNetworkLineEndings(const std::string& text) {
    std::string converted;
    converted.reserve(text.size() + text.size() / 8);
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\n' && (index == 0 || text[index - 1] != '\r')) {
            converted.push_back('\r');
        }
        converted.push_back(text[index]);
    }
    return converted;
}

std::string composeDocument(const SmtpSettings& settings, const MailMessage& message) {
    const std::string from = settings.fromName.empty() ? "<" + settings.fromAddress + ">"
                                                       : "\"" + headerSafe(settings.fromName) +
                                                             "\" <" + settings.fromAddress + ">";

    std::string document;
    document += "Date: " + rfc5322Date() + "\n";
    document += "From: " + from + "\n";
    document += "To: <" + headerSafe(message.to) + ">\n";
    document += "Subject: " + headerSafe(message.subject) + "\n";
    document +=
        "Message-ID: <" + common::randomUuid() + "@" + domainOf(settings.fromAddress) + ">\n";
    document += "MIME-Version: 1.0\n";
    document += "Content-Type: text/plain; charset=utf-8\n";
    document += "Content-Transfer-Encoding: 8bit\n";
    // Nothing here is worth a reply, and an automatic one would go to whoever the deployment
    // sends as. This is what stops an out-of-office answering a verification link.
    document += "Auto-Submitted: auto-generated\n";
    document += "\n";
    document += message.body;

    return withNetworkLineEndings(document);
}

/// What curl reads the message out of.
struct Payload {
    const std::string* document{nullptr};
    std::size_t offset{0};
};

std::size_t readPayload(char* buffer, std::size_t size, std::size_t items, void* userdata) {
    auto* payload = static_cast<Payload*>(userdata);
    const std::size_t capacity = size * items;
    if (capacity == 0 || payload->offset >= payload->document->size()) {
        return 0;
    }

    const std::size_t remaining = payload->document->size() - payload->offset;
    const std::size_t taken = std::min(capacity, remaining);
    std::memcpy(buffer, payload->document->data() + payload->offset, taken);
    payload->offset += taken;
    return taken;
}

std::string urlFor(const SmtpSettings& settings) {
    const std::string scheme = settings.security == SmtpSecurity::Tls ? "smtps://" : "smtp://";
    return scheme + settings.host + ":" + std::to_string(settings.port);
}

} // namespace

SmtpMailSender::SmtpMailSender(SmtpSettings settings)
    : settings_(std::move(settings)),
      worker_(std::make_unique<trantor::EventLoopThread>("smtp")) {
    initCurlOnce();
    worker_->run();
}

SmtpMailSender::~SmtpMailSender() = default;

drogon::Task<VoidResult> SmtpMailSender::send(MailMessage message) const {
    auto* resumeLoop = trantor::EventLoop::getEventLoopOfCurrentThread();

    // The lambda writes into this coroutine's own frame, which stays alive for exactly as long
    // as the call is suspended.
    VoidResult outcome = VoidResult::success();
    co_await drogon::queueInLoopCoro(
        worker_->getLoop(),
        [this, &message, &outcome]() { outcome = deliver(message); },
        resumeLoop);

    co_return outcome;
}

VoidResult SmtpMailSender::deliver(const MailMessage& message) const {
    CURL* handle = curl_easy_init();
    if (handle == nullptr) {
        return VoidResult::failure(common::ErrorCode::DependencyFailure,
                                   "the mail transport could not be initialised");
    }

    const std::string document = composeDocument(settings_, message);
    Payload payload{&document, 0};

    std::array<char, CURL_ERROR_SIZE> errorBuffer{};
    curl_slist* recipients = curl_slist_append(nullptr, ("<" + message.to + ">").c_str());

    const std::string url = urlFor(settings_);
    const std::string envelopeFrom = "<" + settings_.fromAddress + ">";

    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_MAIL_FROM, envelopeFrom.c_str());
    curl_easy_setopt(handle, CURLOPT_MAIL_RCPT, recipients);
    curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(handle, CURLOPT_READFUNCTION, readPayload);
    curl_easy_setopt(handle, CURLOPT_READDATA, &payload);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer.data());
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, static_cast<long>(settings_.timeout.count()));
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, static_cast<long>(settings_.timeout.count()));

    // CURLUSESSL_ALL makes STARTTLS mandatory rather than opportunistic: a relay that does not
    // offer it fails the send instead of quietly carrying credentials in the clear.
    if (settings_.security == SmtpSecurity::StartTls) {
        curl_easy_setopt(handle, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
    }

    // Peer and host verification are left at their defaults, which is on. There is no
    // configuration switch to turn them off: a deployment that needs a private certificate
    // authority installs it in the image's trust store, which is a decision about the machine
    // rather than one this server should be able to make about somebody else's relay.
    if (!settings_.username.empty()) {
        curl_easy_setopt(handle, CURLOPT_USERNAME, settings_.username.c_str());
        curl_easy_setopt(handle, CURLOPT_PASSWORD, settings_.password.c_str());
    }

    const CURLcode outcome = curl_easy_perform(handle);

    curl_slist_free_all(recipients);
    curl_easy_cleanup(handle);

    if (outcome != CURLE_OK) {
        const std::string detail =
            errorBuffer[0] != '\0' ? errorBuffer.data() : curl_easy_strerror(outcome);
        return VoidResult::failure(common::ErrorCode::DependencyFailure,
                                   "the message could not be delivered: " + detail);
    }

    return VoidResult::success();
}

} // namespace launcher::services
