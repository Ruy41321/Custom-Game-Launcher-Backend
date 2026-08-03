#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace launcher::common {

/// Fixed-capacity token bucket per key, refilled continuously.
///
/// In-process and therefore per-instance: correct for the single-node deployment this
/// project targets, and deliberately not backed by Redis, which would add a service to the
/// compose stack for a launcher serving a handful of testers. If the API is ever scaled out,
/// this becomes a shared-store problem — noted rather than pre-solved.
///
/// The clock is injectable so expiry and refill can be unit tested without sleeping.
class RateLimiter {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// `capacity` requests are allowed immediately; the bucket refills to full over
    /// `refillWindow`.
    RateLimiter(std::size_t capacity, std::chrono::seconds refillWindow, Clock clock = {});

    /// Consumes one token, returning false when the caller is over its limit.
    bool tryAcquire(const std::string& key);

    /// Applies new limits and forgets every bucket. Changed in place rather than by
    /// building a new limiter because callers hold a long-lived reference to this one.
    void reconfigure(std::size_t capacity, std::chrono::seconds refillWindow);

    /// Seconds until the given key regains a token; zero when one is available now.
    std::chrono::seconds retryAfter(const std::string& key);

    /// Drops buckets that have been full and untouched for a while, so a stream of unique
    /// keys cannot grow the map without bound.
    void prune();

    /// Forgets every bucket. Used by the admin surface to lift a throttle manually, and by
    /// integration tests so one test's attempts do not throttle the next.
    void reset();

    std::size_t trackedKeys() const;

  private:
    struct Bucket {
        double tokens{0.0};
        std::chrono::steady_clock::time_point lastRefill;
    };

    void refill(Bucket& bucket, std::chrono::steady_clock::time_point now) const;

    /// Shared by the constructor and reconfigure(); the caller holds the mutex.
    void applyLimits(std::size_t capacity, std::chrono::seconds refillWindow);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
    double capacity_{0.0};
    double tokensPerSecond_{0.0};
    std::chrono::seconds refillWindow_{0};
    Clock clock_;
};

} // namespace launcher::common
