#include "packet_token_bucket.h"

#include <gtest/gtest.h>

using namespace aegis::fastpath;

namespace {

constexpr uint64_t kHz = 1'000'000'000;   // 1 GHz: 1 cycle == 1 ns

BucketParams params(uint64_t rate, uint64_t burst) {
    BucketParams p;
    EXPECT_TRUE(BucketParams::make(kHz, rate, burst, &p));
    return p;
}

}  // namespace

TEST(PacketTokenBucket, RejectsInvalidParams) {
    BucketParams p;
    EXPECT_FALSE(BucketParams::make(0, 10, 10, &p));
    EXPECT_FALSE(BucketParams::make(kHz, 0, 10, &p));
    EXPECT_FALSE(BucketParams::make(kHz, 10, 0, &p));
    EXPECT_FALSE(BucketParams::make(kHz, 10, UINT64_MAX / kHz + 1, &p));
}

TEST(PacketTokenBucket, NewBucketAllowsExactlyBurst) {
    const BucketParams p = params(100, 5);
    PacketTokenBucket b;
    b.init(p, 1000);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(b.consume(p, 1000)) << i;
    }
    EXPECT_FALSE(b.consume(p, 1000));
}

TEST(PacketTokenBucket, RefillsAtRate) {
    const BucketParams p = params(100, 5);   // one token per 10 ms
    PacketTokenBucket b;
    b.init(p, 0);
    for (int i = 0; i < 5; ++i) b.consume(p, 0);
    EXPECT_FALSE(b.consume(p, 0));

    EXPECT_FALSE(b.consume(p, 9'999'999));   // just under one token
    EXPECT_TRUE(b.consume(p, 10'000'000));   // exactly one token
    EXPECT_FALSE(b.consume(p, 10'000'000));
}

TEST(PacketTokenBucket, SustainedRateMatchesConfig) {
    const BucketParams p = params(1000, 10);
    PacketTokenBucket b;
    b.init(p, 0);
    // Offer 10x the rate for 10 seconds: allowed = burst + rate * seconds.
    uint64_t allowed = 0;
    for (uint64_t t = 0; t < 10 * kHz; t += 100'000) {   // 10k pps offered
        allowed += b.consume(p, t);
    }
    EXPECT_NEAR(static_cast<double>(allowed), 10 + 1000 * 10, 2);
}

TEST(PacketTokenBucket, CapsAtCapacityAfterLongIdle) {
    const BucketParams p = params(1'000'000, 3);
    PacketTokenBucket b;
    b.init(p, 0);
    for (int i = 0; i < 3; ++i) b.consume(p, 0);
    // A day of idle at 1 Mpps would overflow elapsed * rate without the clamp.
    const uint64_t later = 86'400ULL * kHz;
    EXPECT_EQ(b.credits, 0u);
    for (int i = 0; i < 3; ++i) EXPECT_TRUE(b.consume(p, later));
    EXPECT_FALSE(b.consume(p, later));
}

TEST(PacketTokenBucket, IgnoresTimeGoingBackwards) {
    const BucketParams p = params(100, 1);
    PacketTokenBucket b;
    b.init(p, 1'000'000'000);
    EXPECT_TRUE(b.consume(p, 1'000'000'000));
    EXPECT_FALSE(b.consume(p, 5));
    EXPECT_EQ(b.last_cycles, 1'000'000'000u);
}

TEST(PacketTokenBucket, IdleMeansFull) {
    const BucketParams p = params(100, 5);   // full refill takes 50 ms
    PacketTokenBucket b;
    b.init(p, 0);
    b.consume(p, 0);
    EXPECT_FALSE(b.is_idle(p, 49'999'999));
    EXPECT_TRUE(b.is_idle(p, 50'000'000));
}
