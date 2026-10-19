#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sw/redis++/redis++.h>
#include "rate-limiting-algorithm/token_bucket.h"
#include "rate-limiting-algorithm/rate_limiter.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
//
// TokenBucket keeps its state in a Redis Cluster, so these are integration
// tests. Point them at a cluster with AEGIS_TEST_REDIS_URI, e.g.
//   AEGIS_TEST_REDIS_URI=tcp://172.20.0.11:6379 bazel test //:token_bucket_test
// If no cluster is reachable, every test is skipped instead of failing.
//
// Note: Redis TIME keeps moving between calls, so stored tokens are fractional
// (e.g. 3.0004). The script floors them, so exact `remaining` checks are only
// used with slow refill rates; fast-refill tests assert bounds instead.

static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static std::string redis_uri() {
    const char* uri = std::getenv("AEGIS_TEST_REDIS_URI");
    return uri ? uri : "tcp://127.0.0.1:7001";
}

class TokenBucketTest : public ::testing::Test {
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

    std::unique_ptr<TokenBucket> make_bucket(int capacity, int refill_rate) {
        return make_bucket(key(), capacity, refill_rate);
    }

    // Explicit key — for tests that share state across instances or pre-seed Redis
    std::unique_ptr<TokenBucket> make_bucket(const std::string& k, int capacity, int refill_rate) {
        return std::make_unique<TokenBucket>(k, capacity, refill_rate, redis_uri());
    }

    std::unique_ptr<sw::redis::RedisCluster> admin_;
    std::vector<std::string> keys_;
};

// ---------------------------------------------------------------------------
// 1. Construction
// ---------------------------------------------------------------------------

TEST_F(TokenBucketTest, FirstConsumeStartsFromFullBucket) {
    auto bucket = make_bucket(5, 1);
    RateLimitDecision d = bucket->consume();
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, 4);
    EXPECT_EQ(d.retry_after_seconds, 0);
}

TEST_F(TokenBucketTest, ThrowsOnZeroCapacity) {
    EXPECT_THROW(make_bucket(0, 2), std::invalid_argument);
}

TEST_F(TokenBucketTest, ThrowsOnNegativeCapacity) {
    EXPECT_THROW(make_bucket(-1, 2), std::invalid_argument);
}

TEST_F(TokenBucketTest, ThrowsOnZeroRefillRate) {
    EXPECT_THROW(make_bucket(5, 0), std::invalid_argument);
}

TEST_F(TokenBucketTest, ThrowsOnNegativeRefillRate) {
    EXPECT_THROW(make_bucket(5, -1), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// 2. Polymorphism — TokenBucket is usable as a RateLimiter
// ---------------------------------------------------------------------------

TEST_F(TokenBucketTest, IsARateLimiter) {
    std::unique_ptr<RateLimiter> limiter = make_bucket(5, 2);
    // Should compile and behave correctly through the base class interface
    RateLimitDecision d = limiter->consume();
    EXPECT_TRUE(d.allowed);
    EXPECT_GE(d.remaining, 0);
}

// ---------------------------------------------------------------------------
// 3. consume() — allowed / remaining
// ---------------------------------------------------------------------------

TEST_F(TokenBucketTest, RemainingDecrementsWithEachConsume) {
    auto bucket = make_bucket(5, 1);
    for (int expected = 4; expected >= 0; --expected) {
        RateLimitDecision d = bucket->consume();
        EXPECT_TRUE(d.allowed) << "Failed when expecting remaining " << expected;
        EXPECT_EQ(d.remaining, expected);
    }
}

TEST_F(TokenBucketTest, ConsumeMultipleTokensAtOnce) {
    auto bucket = make_bucket(10, 1);
    RateLimitDecision d = bucket->consume(3);
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, 7);
}

TEST_F(TokenBucketTest, ConsumeDeniesRequestWhenBucketEmpty) {
    auto bucket = make_bucket(3, 1);
    bucket->consume();
    bucket->consume();
    EXPECT_EQ(bucket->consume().remaining, 0);  // bucket is now empty

    RateLimitDecision d = bucket->consume();
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.remaining, 0);
}

TEST_F(TokenBucketTest, ConsumeAllowsExactCapacityRequests) {
    auto bucket = make_bucket(5, 1);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(bucket->consume().allowed) << "Failed on request " << i + 1;
    }
    EXPECT_FALSE(bucket->consume().allowed);  // 6th should be denied
}

TEST_F(TokenBucketTest, ConsumeDeniedWhenRequestingMoreThanAvailable) {
    auto bucket = make_bucket(3, 1);
    RateLimitDecision d = bucket->consume(5);   // requesting more than capacity
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.remaining, 3);                  // nothing was taken
}

TEST_F(TokenBucketTest, DeniedRequestDoesNotConsumeTokens) {
    auto bucket = make_bucket(10, 1);
    bucket->consume(8);                          // 2 left
    EXPECT_FALSE(bucket->consume(5).allowed);    // too big — denied
    RateLimitDecision d = bucket->consume(2);    // the 2 are still there
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, 0);
}

// ---------------------------------------------------------------------------
// 4. retry_after_seconds
// ---------------------------------------------------------------------------

TEST_F(TokenBucketTest, RetryAfterIsZeroWhenAllowed) {
    auto bucket = make_bucket(5, 1);
    EXPECT_EQ(bucket->consume().retry_after_seconds, 0);
}

TEST_F(TokenBucketTest, RetryAfterIsOneSecondForOneMissingToken) {
    auto bucket = make_bucket(3, 1);             // 1 token/sec
    for (int i = 0; i < 3; ++i) bucket->consume();
    RateLimitDecision d = bucket->consume();
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.retry_after_seconds, 1);         // ceil((1 - ~0) / 1)
}

TEST_F(TokenBucketTest, RetryAfterScalesWithDeficit) {
    auto bucket = make_bucket(10, 1);            // 1 token/sec
    bucket->consume(10);                         // drain
    RateLimitDecision d = bucket->consume(4);    // missing ~4 tokens
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.retry_after_seconds, 4);
}

TEST_F(TokenBucketTest, RetryAfterAccountsForRefillRate) {
    auto bucket = make_bucket(10, 5);            // 5 tokens/sec
    bucket->consume(10);
    RateLimitDecision d = bucket->consume(10);   // missing ~10 tokens at 5/sec
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.retry_after_seconds, 2);
}

// ---------------------------------------------------------------------------
// 5. Refill behaviour
// ---------------------------------------------------------------------------

TEST_F(TokenBucketTest, TokensRefillOverTime) {
    auto bucket = make_bucket(5, 10);  // refills 10 tokens/sec = 1 token per 100ms
    // drain the bucket
    for (int i = 0; i < 5; ++i) bucket->consume();
    EXPECT_FALSE(bucket->consume().allowed);

    sleep_ms(200);  // wait 200ms — should refill ~2 tokens

    RateLimitDecision d = bucket->consume();
    EXPECT_TRUE(d.allowed);
    EXPECT_GE(d.remaining, 1);   // ~2 refilled, 1 consumed
    EXPECT_LE(d.remaining, 2);   // tolerance for scheduler jitter
}

TEST_F(TokenBucketTest, TokensDoNotExceedCapacity) {
    auto bucket = make_bucket(5, 100);  // very fast refill
    bucket->consume();                  // create the key in Redis
    sleep_ms(200);                      // wait well past full (+20 tokens)
    EXPECT_EQ(bucket->consume().remaining, 4);  // capped at 5, minus this one
}

TEST_F(TokenBucketTest, PartialRefillAllowsCorrectNumberOfRequests) {
    auto bucket = make_bucket(5, 2);  // 2 tokens/sec = 1 token per 500ms
    // drain fully
    for (int i = 0; i < 5; ++i) bucket->consume();

    sleep_ms(500);  // wait for ~1 token to refill

    EXPECT_TRUE(bucket->consume().allowed);   // 1 refilled token consumed
    EXPECT_FALSE(bucket->consume().allowed);  // no more tokens
}

// ---------------------------------------------------------------------------
// 6. Fail-open on Redis errors
// ---------------------------------------------------------------------------

TEST_F(TokenBucketTest, FailsOpenWhenRedisScriptErrors) {
    // A plain string at the bucket key makes HGET raise WRONGTYPE inside the script
    std::string k = key();
    admin_->set(k, "not-a-hash");
    auto bucket = make_bucket(k, 5, 1);

    RateLimitDecision d = bucket->consume();
    EXPECT_TRUE(d.allowed);              // fail open
    EXPECT_EQ(d.remaining, -1);          // unknown
    EXPECT_EQ(d.retry_after_seconds, 0);
}

// ---------------------------------------------------------------------------
// 7. Thread safety / distributed state
// ---------------------------------------------------------------------------

TEST_F(TokenBucketTest, ThreadSafeUnderConcurrentConsume) {
    // Slow refill so the capacity bound is actually exercised:
    // 5 threads x 20 requests = 100 attempts against 50 tokens
    auto bucket = make_bucket(50, 1);
    int allowed = 0;
    std::mutex count_mutex;

    auto task = [&]() {
        for (int i = 0; i < 20; ++i) {
            if (bucket->consume().allowed) {
                std::lock_guard<std::mutex> lock(count_mutex);
                ++allowed;
            }
        }
    };

    std::thread t1(task), t2(task), t3(task), t4(task), t5(task);
    t1.join(); t2.join(); t3.join(); t4.join(); t5.join();

    // allowed should never exceed capacity (+1 for refill during the run)
    EXPECT_GE(allowed, 50);
    EXPECT_LE(allowed, 51);
}

TEST_F(TokenBucketTest, SeparateInstancesShareStateThroughRedis) {
    // Simulates two Aegis replicas: no shared C++ mutex, only the atomic Lua script
    std::string k = key();
    auto replica_a = make_bucket(k, 50, 1);
    auto replica_b = make_bucket(k, 50, 1);
    int allowed = 0;
    std::mutex count_mutex;

    auto task = [&](TokenBucket* bucket) {
        for (int i = 0; i < 50; ++i) {
            if (bucket->consume().allowed) {
                std::lock_guard<std::mutex> lock(count_mutex);
                ++allowed;
            }
        }
    };

    std::thread ta(task, replica_a.get()), tb(task, replica_b.get());
    ta.join(); tb.join();

    EXPECT_GE(allowed, 50);
    EXPECT_LE(allowed, 51);
}
