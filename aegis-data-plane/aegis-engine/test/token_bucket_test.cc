#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
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

    // Limiters borrow the fixture's client, the same way production limiters
    // borrow the one RedisCluster created in main(). The key is no longer part
    // of the limiter: each test passes it to consume().
    std::unique_ptr<TokenBucket> make_bucket(int capacity, int refill_rate) {
        return std::make_unique<TokenBucket>(capacity, refill_rate, *admin_);
    }

    std::unique_ptr<sw::redis::RedisCluster> admin_;
    std::vector<std::string> keys_;
};

// ---------------------------------------------------------------------------
// 1. Construction
// ---------------------------------------------------------------------------

TEST_F(TokenBucketTest, FirstConsumeStartsFromFullBucket) {
    auto bucket = make_bucket(5, 1);
    std::string k = key();
    RateLimitDecision d = bucket->consume(k, 1);
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

TEST_F(TokenBucketTest, ConstructionDoesNoNetworkWork) {
    // Limiters now only store 3 fields and borrow the shared client, which is
    // what makes it OK to build one per request. 10,000 constructions should
    // take microseconds; the old constructor opened a RedisCluster each time.
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; ++i) {
        TokenBucket bucket(5, 1, *admin_);
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::milliseconds(100));
}

// ---------------------------------------------------------------------------
// 2. Polymorphism — TokenBucket is usable as a RateLimiter
// ---------------------------------------------------------------------------

TEST_F(TokenBucketTest, IsARateLimiter) {
    std::unique_ptr<RateLimiter> limiter = make_bucket(5, 2);
    std::string k = key();
    // Through the base class, `tokens` uses its default argument (1)
    RateLimitDecision d = limiter->consume(k);
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, 4);
}

// ---------------------------------------------------------------------------
// 3. consume() — allowed / remaining
// ---------------------------------------------------------------------------

TEST_F(TokenBucketTest, RemainingDecrementsWithEachConsume) {
    auto bucket = make_bucket(5, 1);
    std::string k = key();
    for (int expected = 4; expected >= 0; --expected) {
        RateLimitDecision d = bucket->consume(k, 1);
        EXPECT_TRUE(d.allowed) << "Failed when expecting remaining " << expected;
        EXPECT_EQ(d.remaining, expected);
    }
}

TEST_F(TokenBucketTest, ConsumeMultipleTokensAtOnce) {
    auto bucket = make_bucket(10, 1);
    std::string k = key();
    RateLimitDecision d = bucket->consume(k, 3);
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, 7);
}

TEST_F(TokenBucketTest, ConsumeDeniesRequestWhenBucketEmpty) {
    auto bucket = make_bucket(3, 1);
    std::string k = key();
    bucket->consume(k, 1);
    bucket->consume(k, 1);
    EXPECT_EQ(bucket->consume(k, 1).remaining, 0);  // bucket is now empty

    RateLimitDecision d = bucket->consume(k, 1);
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.remaining, 0);
}

TEST_F(TokenBucketTest, ConsumeAllowsExactCapacityRequests) {
    auto bucket = make_bucket(5, 1);
    std::string k = key();
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(bucket->consume(k, 1).allowed) << "Failed on request " << i + 1;
    }
    EXPECT_FALSE(bucket->consume(k, 1).allowed);  // 6th should be denied
}

TEST_F(TokenBucketTest, ConsumeDeniedWhenRequestingMoreThanAvailable) {
    auto bucket = make_bucket(3, 1);
    std::string k = key();
    RateLimitDecision d = bucket->consume(k, 5);   // requesting more than capacity
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.remaining, 3);                     // nothing was taken
}

TEST_F(TokenBucketTest, DeniedRequestDoesNotConsumeTokens) {
    auto bucket = make_bucket(10, 1);
    std::string k = key();
    bucket->consume(k, 8);                          // 2 left
    EXPECT_FALSE(bucket->consume(k, 5).allowed);    // too big — denied
    RateLimitDecision d = bucket->consume(k, 2);    // the 2 are still there
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, 0);
}

// ---------------------------------------------------------------------------
// 4. The key is passed to consume(), not stored in the limiter
// ---------------------------------------------------------------------------

TEST_F(TokenBucketTest, OneInstanceServesManyKeysIndependently) {
    // One limiter per rule can now serve every caller of that rule.
    // Draining ip A's bucket must not touch ip B's.
    auto bucket = make_bucket(2, 1);
    std::string ip_a = key();
    std::string ip_b = key();

    EXPECT_TRUE(bucket->consume(ip_a, 1).allowed);
    EXPECT_TRUE(bucket->consume(ip_a, 1).allowed);
    EXPECT_FALSE(bucket->consume(ip_a, 1).allowed);   // A is out of tokens

    RateLimitDecision d = bucket->consume(ip_b, 1);    // B still has a full bucket
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, 1);
}

TEST_F(TokenBucketTest, StateLivesInRedisNotInTheLimiterObject) {
    // This is why the limiters_ cache could be deleted: the server builds a
    // fresh limiter per request, and the count must carry over between them.
    std::string k = key();
    for (int expected = 2; expected >= 0; --expected) {
        auto per_request = make_bucket(3, 1);          // new object every time
        RateLimitDecision d = per_request->consume(k, 1);
        EXPECT_TRUE(d.allowed);
        EXPECT_EQ(d.remaining, expected);
    }                                                  // ...destroyed every time
    EXPECT_FALSE(make_bucket(3, 1)->consume(k, 1).allowed);
}

TEST_F(TokenBucketTest, ConsumeWritesOnlyTheGivenKey) {
    std::string k = key();
    std::string untouched = key();
    auto bucket = make_bucket(5, 1);
    bucket->consume(k, 1);
    EXPECT_EQ(admin_->exists(k), 1);
    EXPECT_EQ(admin_->exists(untouched), 0);
}

// ---------------------------------------------------------------------------
// 5. retry_after_seconds
// ---------------------------------------------------------------------------

TEST_F(TokenBucketTest, RetryAfterIsZeroWhenAllowed) {
    auto bucket = make_bucket(5, 1);
    std::string k = key();
    EXPECT_EQ(bucket->consume(k, 1).retry_after_seconds, 0);
}

TEST_F(TokenBucketTest, RetryAfterIsOneSecondForOneMissingToken) {
    auto bucket = make_bucket(3, 1);             // 1 token/sec
    std::string k = key();
    for (int i = 0; i < 3; ++i) bucket->consume(k, 1);
    RateLimitDecision d = bucket->consume(k, 1);
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.retry_after_seconds, 1);         // ceil((1 - ~0) / 1)
}

TEST_F(TokenBucketTest, RetryAfterScalesWithDeficit) {
    auto bucket = make_bucket(10, 1);            // 1 token/sec
    std::string k = key();
    bucket->consume(k, 10);                      // drain
    RateLimitDecision d = bucket->consume(k, 4); // missing ~4 tokens
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.retry_after_seconds, 4);
}

TEST_F(TokenBucketTest, RetryAfterAccountsForRefillRate) {
    auto bucket = make_bucket(10, 5);            // 5 tokens/sec
    std::string k = key();
    bucket->consume(k, 10);
    RateLimitDecision d = bucket->consume(k, 10); // missing ~10 tokens at 5/sec
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.retry_after_seconds, 2);
}

// ---------------------------------------------------------------------------
// 6. Refill behaviour
// ---------------------------------------------------------------------------

TEST_F(TokenBucketTest, TokensRefillOverTime) {
    auto bucket = make_bucket(5, 10);  // refills 10 tokens/sec = 1 token per 100ms
    std::string k = key();
    // drain the bucket
    for (int i = 0; i < 5; ++i) bucket->consume(k, 1);
    EXPECT_FALSE(bucket->consume(k, 1).allowed);

    sleep_ms(200);  // wait 200ms — should refill ~2 tokens

    RateLimitDecision d = bucket->consume(k, 1);
    EXPECT_TRUE(d.allowed);
    EXPECT_GE(d.remaining, 1);   // ~2 refilled, 1 consumed
    EXPECT_LE(d.remaining, 2);   // tolerance for scheduler jitter
}

TEST_F(TokenBucketTest, TokensDoNotExceedCapacity) {
    auto bucket = make_bucket(5, 100);  // very fast refill
    std::string k = key();
    bucket->consume(k, 1);              // create the key in Redis
    sleep_ms(200);                      // wait well past full (+20 tokens)
    EXPECT_EQ(bucket->consume(k, 1).remaining, 4);  // capped at 5, minus this one
}

TEST_F(TokenBucketTest, PartialRefillAllowsCorrectNumberOfRequests) {
    auto bucket = make_bucket(5, 2);  // 2 tokens/sec = 1 token per 500ms
    std::string k = key();
    // drain fully
    for (int i = 0; i < 5; ++i) bucket->consume(k, 1);

    sleep_ms(500);  // wait for ~1 token to refill

    EXPECT_TRUE(bucket->consume(k, 1).allowed);   // 1 refilled token consumed
    EXPECT_FALSE(bucket->consume(k, 1).allowed);  // no more tokens
}

// ---------------------------------------------------------------------------
// 7. Fail-open on Redis errors
// ---------------------------------------------------------------------------

TEST_F(TokenBucketTest, FailsOpenWhenRedisScriptErrors) {
    // A plain string at the bucket key makes HGET raise WRONGTYPE inside the script
    std::string k = key();
    admin_->set(k, "not-a-hash");
    auto bucket = make_bucket(5, 1);

    RateLimitDecision d = bucket->consume(k, 1);
    EXPECT_TRUE(d.allowed);              // fail open
    EXPECT_EQ(d.remaining, -1);          // unknown
    EXPECT_EQ(d.retry_after_seconds, 0);
}

TEST_F(TokenBucketTest, BadKeyDoesNotAffectOtherKeysOnTheSameLimiter) {
    // With one shared limiter, an error on one caller's key must not leak
    // into the next call for a different caller.
    std::string bad = key();
    std::string good = key();
    admin_->set(bad, "not-a-hash");
    auto bucket = make_bucket(5, 1);

    EXPECT_EQ(bucket->consume(bad, 1).remaining, -1);   // failed open
    RateLimitDecision d = bucket->consume(good, 1);
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, 4);                          // a real answer
}

// ---------------------------------------------------------------------------
// 8. Thread safety / distributed state
// ---------------------------------------------------------------------------

TEST_F(TokenBucketTest, ThreadSafeUnderConcurrentConsumeOnOneInstance) {
    // TokenBucket no longer has a mutex: 5 threads share ONE instance and ONE
    // client. The atomic Lua script must still hand out at most 50 tokens.
    // Slow refill so the capacity bound is actually exercised:
    // 5 threads x 20 requests = 100 attempts against 50 tokens.
    auto bucket = make_bucket(50, 1);
    std::string k = key();
    std::atomic<int> allowed{0};

    auto task = [&]() {
        for (int i = 0; i < 20; ++i) {
            if (bucket->consume(k, 1).allowed) ++allowed;
        }
    };

    std::thread t1(task), t2(task), t3(task), t4(task), t5(task);
    t1.join(); t2.join(); t3.join(); t4.join(); t5.join();

    // allowed should never exceed capacity (+1 for refill during the run)
    EXPECT_GE(allowed.load(), 50);
    EXPECT_LE(allowed.load(), 51);
}

TEST_F(TokenBucketTest, ThreadSafeWithALimiterPerRequest) {
    // Mirrors the server: every request builds its own short-lived limiter,
    // and all of them share one client and one Redis key.
    std::string k = key();
    std::atomic<int> allowed{0};

    auto task = [&]() {
        for (int i = 0; i < 20; ++i) {
            auto per_request = make_bucket(50, 1);
            if (per_request->consume(k, 1).allowed) ++allowed;
        }
    };

    std::thread t1(task), t2(task), t3(task), t4(task), t5(task);
    t1.join(); t2.join(); t3.join(); t4.join(); t5.join();

    EXPECT_GE(allowed.load(), 50);
    EXPECT_LE(allowed.load(), 51);
}

TEST_F(TokenBucketTest, SeparateReplicasShareStateThroughRedis) {
    // Simulates two Aegis replicas: each has its OWN RedisCluster client (like
    // two processes), no shared C++ state, only the atomic Lua script.
    sw::redis::RedisCluster other_replica_client(redis_uri());
    std::string k = key();
    TokenBucket replica_a(50, 1, *admin_);
    TokenBucket replica_b(50, 1, other_replica_client);
    std::atomic<int> allowed{0};

    auto task = [&](TokenBucket* bucket) {
        for (int i = 0; i < 50; ++i) {
            if (bucket->consume(k, 1).allowed) ++allowed;
        }
    };

    std::thread ta(task, &replica_a), tb(task, &replica_b);
    ta.join(); tb.join();

    EXPECT_GE(allowed.load(), 50);
    EXPECT_LE(allowed.load(), 51);
}
