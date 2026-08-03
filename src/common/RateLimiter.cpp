#include "common/RateLimiter.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace launcher::common {
namespace {

constexpr std::size_t MINIMUM_CAPACITY = 1;

} // namespace

RateLimiter::RateLimiter(std::size_t capacity, std::chrono::seconds refillWindow, Clock clock)
    : clock_(clock ? std::move(clock) : []() { return std::chrono::steady_clock::now(); }) {
    applyLimits(capacity, refillWindow);
}

void RateLimiter::applyLimits(std::size_t capacity, std::chrono::seconds refillWindow) {
    capacity_ = static_cast<double>(std::max(capacity, MINIMUM_CAPACITY));
    refillWindow_ = refillWindow.count() > 0 ? refillWindow : std::chrono::seconds{1};
    tokensPerSecond_ = capacity_ / static_cast<double>(refillWindow_.count());
}

void RateLimiter::reconfigure(std::size_t capacity, std::chrono::seconds refillWindow) {
    const std::lock_guard<std::mutex> lock(mutex_);
    applyLimits(capacity, refillWindow);
    buckets_.clear();
}

void RateLimiter::refill(Bucket& bucket, std::chrono::steady_clock::time_point now) const {
    const auto elapsed = std::chrono::duration<double>(now - bucket.lastRefill).count();
    if (elapsed > 0.0) {
        bucket.tokens = std::min(capacity_, bucket.tokens + elapsed * tokensPerSecond_);
        bucket.lastRefill = now;
    }
}

bool RateLimiter::tryAcquire(const std::string& key) {
    const auto now = clock_();
    const std::lock_guard<std::mutex> lock(mutex_);

    auto [entry, inserted] = buckets_.try_emplace(key, Bucket{capacity_, now});
    if (!inserted) {
        refill(entry->second, now);
    }

    if (entry->second.tokens < 1.0) {
        return false;
    }

    entry->second.tokens -= 1.0;
    return true;
}

std::chrono::seconds RateLimiter::retryAfter(const std::string& key) {
    const auto now = clock_();
    const std::lock_guard<std::mutex> lock(mutex_);

    const auto entry = buckets_.find(key);
    if (entry == buckets_.end()) {
        return std::chrono::seconds{0};
    }

    refill(entry->second, now);
    if (entry->second.tokens >= 1.0) {
        return std::chrono::seconds{0};
    }

    const auto missing = 1.0 - entry->second.tokens;
    return std::chrono::seconds{static_cast<long long>(std::ceil(missing / tokensPerSecond_))};
}

void RateLimiter::prune() {
    const auto now = clock_();
    const std::lock_guard<std::mutex> lock(mutex_);

    for (auto entry = buckets_.begin(); entry != buckets_.end();) {
        refill(entry->second, now);
        // A full bucket is indistinguishable from one that never existed.
        entry = entry->second.tokens >= capacity_ ? buckets_.erase(entry) : std::next(entry);
    }
}

void RateLimiter::reset() {
    const std::lock_guard<std::mutex> lock(mutex_);
    buckets_.clear();
}

std::size_t RateLimiter::trackedKeys() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return buckets_.size();
}

} // namespace launcher::common
