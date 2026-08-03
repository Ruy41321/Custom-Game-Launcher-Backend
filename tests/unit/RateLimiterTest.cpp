#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "common/RateLimiter.h"

namespace {

using launcher::common::RateLimiter;
using namespace std::chrono_literals;

/// A clock the test drives by hand, so refill and expiry are exercised without sleeping.
class FakeClock {
  public:
    std::chrono::steady_clock::time_point now() const { return origin_ + elapsed_; }

    void advance(std::chrono::milliseconds by) { elapsed_ += by; }

    RateLimiter::Clock asClock() {
        return [this]() { return now(); };
    }

  private:
    std::chrono::steady_clock::time_point origin_{std::chrono::steady_clock::now()};
    std::chrono::milliseconds elapsed_{0};
};

TEST(RateLimiterTest, AllowsUpToCapacityImmediately) {
    FakeClock clock;
    RateLimiter limiter(5, 60s, clock.asClock());

    for (int attempt = 0; attempt < 5; ++attempt) {
        EXPECT_TRUE(limiter.tryAcquire("1.2.3.4")) << "attempt " << attempt;
    }
}

TEST(RateLimiterTest, RefusesOnceTheBucketIsEmpty) {
    FakeClock clock;
    RateLimiter limiter(3, 60s, clock.asClock());

    for (int attempt = 0; attempt < 3; ++attempt) {
        ASSERT_TRUE(limiter.tryAcquire("1.2.3.4"));
    }

    EXPECT_FALSE(limiter.tryAcquire("1.2.3.4"));
    EXPECT_FALSE(limiter.tryAcquire("1.2.3.4"));
}

// One noisy address must not lock everybody else out.
TEST(RateLimiterTest, TracksEachKeySeparately) {
    FakeClock clock;
    RateLimiter limiter(2, 60s, clock.asClock());

    ASSERT_TRUE(limiter.tryAcquire("1.2.3.4"));
    ASSERT_TRUE(limiter.tryAcquire("1.2.3.4"));
    ASSERT_FALSE(limiter.tryAcquire("1.2.3.4"));

    EXPECT_TRUE(limiter.tryAcquire("5.6.7.8"));
    EXPECT_TRUE(limiter.tryAcquire("5.6.7.8"));
}

TEST(RateLimiterTest, RefillsGraduallyRatherThanAllAtOnce) {
    FakeClock clock;
    // 10 per 10 seconds, i.e. one token per second.
    RateLimiter limiter(10, 10s, clock.asClock());

    for (int attempt = 0; attempt < 10; ++attempt) {
        ASSERT_TRUE(limiter.tryAcquire("client"));
    }
    ASSERT_FALSE(limiter.tryAcquire("client"));

    clock.advance(1100ms);
    EXPECT_TRUE(limiter.tryAcquire("client")) << "one token should have been refilled";
    EXPECT_FALSE(limiter.tryAcquire("client")) << "but only one";

    clock.advance(3000ms);
    EXPECT_TRUE(limiter.tryAcquire("client"));
    EXPECT_TRUE(limiter.tryAcquire("client"));
    EXPECT_TRUE(limiter.tryAcquire("client"));
    EXPECT_FALSE(limiter.tryAcquire("client"));
}

TEST(RateLimiterTest, NeverRefillsBeyondCapacity) {
    FakeClock clock;
    RateLimiter limiter(3, 10s, clock.asClock());

    ASSERT_TRUE(limiter.tryAcquire("client"));
    clock.advance(10min); // far longer than the window

    EXPECT_TRUE(limiter.tryAcquire("client"));
    EXPECT_TRUE(limiter.tryAcquire("client"));
    EXPECT_TRUE(limiter.tryAcquire("client"));
    EXPECT_FALSE(limiter.tryAcquire("client")) << "the bucket must cap at its capacity";
}

TEST(RateLimiterTest, ReportsWhenToRetry) {
    FakeClock clock;
    RateLimiter limiter(2, 10s, clock.asClock());

    EXPECT_EQ(limiter.retryAfter("client"), 0s) << "an untouched key can go right away";

    ASSERT_TRUE(limiter.tryAcquire("client"));
    EXPECT_EQ(limiter.retryAfter("client"), 0s) << "a token is still available";

    ASSERT_TRUE(limiter.tryAcquire("client"));
    ASSERT_FALSE(limiter.tryAcquire("client"));

    const auto wait = limiter.retryAfter("client");
    EXPECT_GT(wait, 0s);
    EXPECT_LE(wait, 10s);

    clock.advance(std::chrono::duration_cast<std::chrono::milliseconds>(wait) + 50ms);
    EXPECT_TRUE(limiter.tryAcquire("client"));
}

// A stream of unique addresses must not be a way to exhaust the server's memory.
TEST(RateLimiterTest, PruneDropsRecoveredBuckets) {
    FakeClock clock;
    RateLimiter limiter(2, 10s, clock.asClock());

    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(limiter.tryAcquire("client-" + std::to_string(i)));
    }
    EXPECT_EQ(limiter.trackedKeys(), 50U);

    limiter.prune();
    EXPECT_EQ(limiter.trackedKeys(), 50U) << "buckets still below capacity must be kept";

    clock.advance(30s);
    limiter.prune();
    EXPECT_EQ(limiter.trackedKeys(), 0U) << "fully refilled buckets carry no information";
}

TEST(RateLimiterTest, PruneKeepsThrottledClientsThrottled) {
    FakeClock clock;
    RateLimiter limiter(2, 60s, clock.asClock());

    ASSERT_TRUE(limiter.tryAcquire("client"));
    ASSERT_TRUE(limiter.tryAcquire("client"));
    ASSERT_FALSE(limiter.tryAcquire("client"));

    limiter.prune();

    EXPECT_FALSE(limiter.tryAcquire("client")) << "pruning must not hand out a fresh bucket";
}

TEST(RateLimiterTest, ResetClearsEveryBucket) {
    FakeClock clock;
    RateLimiter limiter(1, 60s, clock.asClock());

    ASSERT_TRUE(limiter.tryAcquire("client"));
    ASSERT_FALSE(limiter.tryAcquire("client"));

    limiter.reset();

    EXPECT_EQ(limiter.trackedKeys(), 0U);
    EXPECT_TRUE(limiter.tryAcquire("client"));
}

TEST(RateLimiterTest, TreatsAZeroCapacityAsOne) {
    FakeClock clock;
    RateLimiter limiter(0, 60s, clock.asClock());

    // A capacity of zero would lock the endpoint out entirely; a misconfiguration should
    // throttle hard, not deny service outright.
    EXPECT_TRUE(limiter.tryAcquire("client"));
    EXPECT_FALSE(limiter.tryAcquire("client"));
}

TEST(RateLimiterTest, ToleratesAZeroLengthWindow) {
    FakeClock clock;
    RateLimiter limiter(2, 0s, clock.asClock());

    EXPECT_TRUE(limiter.tryAcquire("client"));
    EXPECT_TRUE(limiter.tryAcquire("client"));
    EXPECT_FALSE(limiter.tryAcquire("client"));
}

} // namespace
