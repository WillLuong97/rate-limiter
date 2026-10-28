#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <sw/redis++/redis++.h>
#include "rate-limiting-algorithm/sliding_window.h"
#include "rate-limiting-algorithm/rate_limiter.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
//
// SlidingWindow keeps its state in a Redis Cluster, so these are integration
// tests. Point them at a cluster with AEGIS_TEST_REDIS_URI, e.g.
//   AEGIS_TEST_REDIS_URI=tcp://172.20.0.11:6379 bazel test //:sliding_window_test
// If no cluster is reachable, every test is skipped instead of failing.
//
// Note: the window only slides when real time passes, so the sliding tests use
// 1-3 second windows and sleep past them. Timing asserts allow for scheduler
// jitter by checking bounds instead of exact values.

static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static std::string redis_uri() {
    const char* uri = std::getenv("AEGIS_TEST_REDIS_URI");
    return uri ? uri : "tcp://127.0.0.1:7001";
}

static long long now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

class SlidingWindowTest : public ::testing::Test {
protected:
    void SetUp() override {
        try {
            admin_ = std::make_unique<sw::redis::RedisCluster>(redis_uri());
            admin_->exists("aegis:test:ping");   // force a round trip
        } catch (const sw::redis::Error& e) {
            GTEST_SKIP() << "Redis cluster unreachable at " << redis_uri()
                         << " (" << e.what() << ")";
        }
    }

    void TearDown() override {
        if (!admin_) return;
        for (const auto& key : keys_) admin_->del(key);
    }

    // Unique Redis key per test (and per run) so state never leaks between tests
    std::string key() {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        std::string k = std::string("aegis:test:") + info->name() + ":" +
                        std::to_string(now) + ":" + std::to_string(keys_.size());
        keys_.push_back(k);
        return k;
    }

    std::unique_ptr<SlidingWindow> make_window(int capacity, int window_seconds) {
        return make_window(key(), capacity, window_seconds);
    }

    // Explicit key — for tests that inspect Redis or share state across instances
    std::unique_ptr<SlidingWindow> make_window(const std::string& k, int capacity, int window_seconds) {
        return std::make_unique<SlidingWindow>(k, capacity, window_seconds, redis_uri());
    }

    // All {member, score} pairs in the sorted set, oldest first
    std::vector<std::pair<std::string, double>> entries(const std::string& k) {
        std::vector<std::pair<std::string, double>> out;
        admin_->zrange(k, 0, -1, std::back_inserter(out));
        return out;
    }

    std::unique_ptr<sw::redis::RedisCluster> admin_;
    std::vector<std::string> keys_;
};

// ---------------------------------------------------------------------------
// 1. Construction
// ---------------------------------------------------------------------------

TEST_F(SlidingWindowTest, FirstConsumeStartsFromEmptyWindow) {
    auto window = make_window(5, 60);
    RateLimitDecision d = window->consume(1);
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, 4);
    EXPECT_EQ(d.retry_after_seconds, 0);
}

TEST_F(SlidingWindowTest, ThrowsOnZeroCapacity) {
    EXPECT_THROW(make_window(0, 60), std::invalid_argument);
}

TEST_F(SlidingWindowTest, ThrowsOnNegativeCapacity) {
    EXPECT_THROW(make_window(-1, 60), std::invalid_argument);
}

TEST_F(SlidingWindowTest, ThrowsOnZeroWindow) {
    EXPECT_THROW(make_window(5, 0), std::invalid_argument);
}

TEST_F(SlidingWindowTest, ThrowsOnNegativeWindow) {
    EXPECT_THROW(make_window(5, -1), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// 2. Polymorphism — SlidingWindow is usable as a RateLimiter
// ---------------------------------------------------------------------------

TEST_F(SlidingWindowTest, IsARateLimiter) {
    std::unique_ptr<RateLimiter> limiter = make_window(5, 60);
    // Uses the base class default argument (1 token)
    RateLimitDecision d = limiter->consume();
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, 4);
}

// ---------------------------------------------------------------------------
// 3. consume() — allowed / remaining
// ---------------------------------------------------------------------------

TEST_F(SlidingWindowTest, RemainingDecrementsWithEachConsume) {
    auto window = make_window(5, 60);
    for (int expected = 4; expected >= 0; --expected) {
        RateLimitDecision d = window->consume(1);
        EXPECT_TRUE(d.allowed) << "Failed when expecting remaining " << expected;
        EXPECT_EQ(d.remaining, expected);
    }
}

TEST_F(SlidingWindowTest, ConsumeAllowsExactCapacityRequests) {
    auto window = make_window(5, 60);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(window->consume(1).allowed) << "Failed on request " << i + 1;
    }
    EXPECT_FALSE(window->consume(1).allowed);  // 6th should be denied
}

TEST_F(SlidingWindowTest, RejectedRemainingIsZeroNotNegative) {
    // The gRPC server only sends X-RateLimit-Remaining when remaining >= 0
    auto window = make_window(3, 60);
    for (int i = 0; i < 3; ++i) window->consume(1);
    RateLimitDecision d = window->consume(1);
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.remaining, 0);
}

TEST_F(SlidingWindowTest, ConsumeMultipleTokensAtOnce) {
    auto window = make_window(10, 60);
    RateLimitDecision d = window->consume(3);
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, 7);
}

TEST_F(SlidingWindowTest, MultiTokenConsumeIsAllOrNothing) {
    auto window = make_window(5, 60);
    EXPECT_TRUE(window->consume(3).allowed);    // 2 left

    RateLimitDecision d = window->consume(3);   // 3 + 3 > 5
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.remaining, 2);                  // nothing was recorded

    d = window->consume(2);                     // the 2 are still there
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, 0);
}

TEST_F(SlidingWindowTest, ConsumeDeniedWhenRequestingMoreThanCapacity) {
    auto window = make_window(3, 60);
    RateLimitDecision d = window->consume(5);
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.remaining, 3);                  // empty window, nothing taken
}

// ---------------------------------------------------------------------------
// 4. What gets stored in Redis
// ---------------------------------------------------------------------------

TEST_F(SlidingWindowTest, StoresOneEntryPerAllowedToken) {
    std::string k = key();
    auto window = make_window(k, 10, 60);
    window->consume(1);
    window->consume(3);
    // ZCARD drives the decision, so every token needs its own unique member
    EXPECT_EQ(admin_->zcard(k), 4);
}

TEST_F(SlidingWindowTest, RejectedRequestsAreNotStored) {
    std::string k = key();
    auto window = make_window(k, 2, 60);
    window->consume(1);
    window->consume(1);
    for (int i = 0; i < 5; ++i) EXPECT_FALSE(window->consume(1).allowed);
    EXPECT_EQ(admin_->zcard(k), 2);
}

TEST_F(SlidingWindowTest, ScoresAreRedisTimeInMilliseconds) {
    std::string k = key();
    auto window = make_window(k, 5, 60);
    long long before = now_unix_ms();
    window->consume(1);
    long long after = now_unix_ms();

    auto e = entries(k);
    ASSERT_EQ(e.size(), 1u);
    // Redis runs on the same host, so its clock should be within a few seconds
    EXPECT_GE(static_cast<long long>(e[0].second), before - 5000);
    EXPECT_LE(static_cast<long long>(e[0].second), after + 5000);
}

TEST_F(SlidingWindowTest, KeyExpiresAfterWindow) {
    std::string k = key();
    auto window = make_window(k, 5, 60);
    window->consume(1);
    long long ttl_ms = admin_->pttl(k);
    EXPECT_GT(ttl_ms, 0);                      // a TTL is set
    EXPECT_LE(ttl_ms, 60 * 1000);              // and it is the window, in ms
    EXPECT_GT(ttl_ms, 55 * 1000);
}

// ---------------------------------------------------------------------------
// 5. retry_after_seconds
// ---------------------------------------------------------------------------

TEST_F(SlidingWindowTest, RetryAfterIsZeroWhenAllowed) {
    auto window = make_window(5, 60);
    EXPECT_EQ(window->consume(1).retry_after_seconds, 0);
}

TEST_F(SlidingWindowTest, RetryAfterIsFullWindowRightAfterFilling) {
    auto window = make_window(3, 60);
    for (int i = 0; i < 3; ++i) window->consume(1);
    RateLimitDecision d = window->consume(1);   // oldest entry is ~0s old
    EXPECT_FALSE(d.allowed);
    EXPECT_GE(d.retry_after_seconds, 59);
    EXPECT_LE(d.retry_after_seconds, 60);
}

TEST_F(SlidingWindowTest, RetryAfterShrinksAsOldestEntryAges) {
    auto window = make_window(1, 3);            // 1 request per 3s
    window->consume(1);
    sleep_ms(1500);                             // oldest entry is now ~1.5s old
    RateLimitDecision d = window->consume(1);
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.retry_after_seconds, 2);        // ceil(3 - 1.5)
}

TEST_F(SlidingWindowTest, RetryAfterIsWindowWhenRequestExceedsCapacity) {
    // Empty window, so there is no oldest entry to wait for
    auto window = make_window(3, 60);
    RateLimitDecision d = window->consume(5);
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.retry_after_seconds, 60);
}

// ---------------------------------------------------------------------------
// 6. Sliding behaviour
// ---------------------------------------------------------------------------

TEST_F(SlidingWindowTest, SlotsFreeUpAfterWindowPasses) {
    auto window = make_window(2, 1);            // 2 requests per 1s
    EXPECT_TRUE(window->consume(1).allowed);
    EXPECT_TRUE(window->consume(1).allowed);
    EXPECT_FALSE(window->consume(1).allowed);

    sleep_ms(1100);                             // both entries slide out

    RateLimitDecision d = window->consume(1);
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, 1);
}

TEST_F(SlidingWindowTest, SlotsFreeUpOneAtATime) {
    auto window = make_window(2, 2);            // 2 requests per 2s
    EXPECT_TRUE(window->consume(1).allowed);    // t = 0s
    sleep_ms(1000);
    EXPECT_TRUE(window->consume(1).allowed);    // t = 1s
    EXPECT_FALSE(window->consume(1).allowed);   // full

    sleep_ms(1100);                             // t = 2.1s: only the t=0s entry has left

    EXPECT_TRUE(window->consume(1).allowed);    // takes the freed slot
    EXPECT_FALSE(window->consume(1).allowed);   // t=1s entry is still in the window
}

TEST_F(SlidingWindowTest, RejectedRequestsDoNotExtendLockout) {
    auto window = make_window(1, 1);            // 1 request per 1s
    EXPECT_TRUE(window->consume(1).allowed);
    for (int i = 0; i < 5; ++i) {               // hammer while locked out
        EXPECT_FALSE(window->consume(1).allowed);
        sleep_ms(100);
    }
    sleep_ms(600);                              // ~1.1s after the allowed request
    EXPECT_TRUE(window->consume(1).allowed);
}

TEST_F(SlidingWindowTest, SeparateKeysAreIndependent) {
    // e.g. ip:A:login vs ip:B:login — one IP hitting its limit doesn't affect another
    auto window_a = make_window(1, 60);
    auto window_b = make_window(1, 60);
    EXPECT_TRUE(window_a->consume(1).allowed);
    EXPECT_FALSE(window_a->consume(1).allowed);
    EXPECT_TRUE(window_b->consume(1).allowed);
}

// ---------------------------------------------------------------------------
// 7. Fail-open on Redis errors
// ---------------------------------------------------------------------------

TEST_F(SlidingWindowTest, FailsOpenWhenRedisScriptErrors) {
    // A plain string at the key makes ZREMRANGEBYSCORE raise WRONGTYPE inside the script
    std::string k = key();
    admin_->set(k, "not-a-sorted-set");
    auto window = make_window(k, 5, 60);

    RateLimitDecision d = window->consume(1);
    EXPECT_TRUE(d.allowed);              // fail open
    EXPECT_EQ(d.remaining, -1);          // unknown
    EXPECT_EQ(d.retry_after_seconds, 0);
}

// ---------------------------------------------------------------------------
// 8. Thread safety / distributed state
// ---------------------------------------------------------------------------

TEST_F(SlidingWindowTest, ThreadSafeUnderConcurrentConsume) {
    // 5 threads x 20 requests = 100 attempts against a limit of 50.
    // Nothing slides out of a 60s window during the run, so the bound is exact.
    std::string k = key();
    auto window = make_window(k, 50, 60);
    int allowed = 0;
    std::mutex count_mutex;

    auto task = [&]() {
        for (int i = 0; i < 20; ++i) {
            if (window->consume(1).allowed) {
                std::lock_guard<std::mutex> lock(count_mutex);
                ++allowed;
            }
        }
    };

    std::thread t1(task), t2(task), t3(task), t4(task), t5(task);
    t1.join(); t2.join(); t3.join(); t4.join(); t5.join();

    EXPECT_EQ(allowed, 50);
    EXPECT_EQ(admin_->zcard(k), 50);     // no two requests collided on the same member
}

TEST_F(SlidingWindowTest, SeparateInstancesShareStateThroughRedis) {
    // Simulates two Aegis replicas: no shared C++ state, only the atomic Lua script
    std::string k = key();
    auto replica_a = make_window(k, 50, 60);
    auto replica_b = make_window(k, 50, 60);
    int allowed = 0;
    std::mutex count_mutex;

    auto task = [&](SlidingWindow* window) {
        for (int i = 0; i < 50; ++i) {
            if (window->consume(1).allowed) {
                std::lock_guard<std::mutex> lock(count_mutex);
                ++allowed;
            }
        }
    };

    std::thread ta(task, replica_a.get()), tb(task, replica_b.get());
    ta.join(); tb.join();

    EXPECT_EQ(allowed, 50);
    EXPECT_EQ(admin_->zcard(k), 50);
}
