#include "flow_table.h"

#include <gtest/gtest.h>

#include <set>
#include <vector>

using namespace aegis::fastpath;

namespace {

constexpr uint64_t kHz = 1'000'000'000;

struct TableFixture : ::testing::Test {
    explicit TableFixture(uint32_t cap = 1024) : slots(cap), table(slots.data(), cap, 42) {
        BucketParams::make(kHz, 100, 10, &p);   // full refill: 100 ms
    }
    std::vector<FlowSlot> slots;
    FlowTable table;
    BucketParams p;
};

}  // namespace

TEST(FlowTableCapacity, PowerOfTwoOnly) {
    EXPECT_TRUE(FlowTable::valid_capacity(16));
    EXPECT_TRUE(FlowTable::valid_capacity(1 << 20));
    EXPECT_FALSE(FlowTable::valid_capacity(8));      // smaller than probe window
    EXPECT_FALSE(FlowTable::valid_capacity(1000));
    EXPECT_FALSE(FlowTable::valid_capacity(1ULL << 32));
}

TEST_F(TableFixture, InsertThenFindSameBucket) {
    LookupStatus st;
    PacketTokenBucket* a = table.find_or_insert(0x01020304, p, 0, &st);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(st, LookupStatus::kInserted);
    PacketTokenBucket* b = table.find_or_insert(0x01020304, p, 5, &st);
    EXPECT_EQ(st, LookupStatus::kFound);
    EXPECT_EQ(a, b);
}

TEST_F(TableFixture, DistinctSourcesGetDistinctBuckets) {
    std::set<PacketTokenBucket*> seen;
    LookupStatus st;
    for (uint32_t ip = 1; ip <= 500; ++ip) {
        PacketTokenBucket* b = table.find_or_insert(ip, p, 0, &st);
        ASSERT_NE(b, nullptr) << ip;
        EXPECT_TRUE(seen.insert(b).second);
    }
    for (uint32_t ip = 1; ip <= 500; ++ip) {
        table.find_or_insert(ip, p, 1, &st);
        EXPECT_EQ(st, LookupStatus::kFound) << ip;
    }
}

TEST_F(TableFixture, FullTableReportsFullWhileSourcesAreActive) {
    LookupStatus st;
    uint32_t inserted = 0;
    for (uint32_t ip = 1; ip <= 4096; ++ip) {
        if (table.find_or_insert(ip, p, 0, &st) != nullptr) ++inserted;
    }
    EXPECT_LE(inserted, table.capacity());
    EXPECT_GT(inserted, table.capacity() / 2);
    EXPECT_EQ(table.find_or_insert(0xdeadbeef, p, 1, &st), nullptr);
    EXPECT_EQ(st, LookupStatus::kFull);
}

TEST_F(TableFixture, IdleSlotsAreRecycled) {
    LookupStatus st;
    for (uint32_t ip = 1; ip <= 4096; ++ip) table.find_or_insert(ip, p, 0, &st);

    // After a full refill period every existing bucket is idle.
    const uint64_t later = p.full_refill_cycles;
    PacketTokenBucket* b = table.find_or_insert(0xdeadbeef, p, later, &st);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(st, LookupStatus::kRecycled);
    EXPECT_EQ(b->credits, p.capacity_credits);

    table.find_or_insert(0xdeadbeef, p, later + 1, &st);
    EXPECT_EQ(st, LookupStatus::kFound);
}

TEST_F(TableFixture, RecycledSourceComesBackWithFreshBucket) {
    LookupStatus st;
    for (uint32_t ip = 1; ip <= 4096; ++ip) table.find_or_insert(ip, p, 0, &st);
    // Evict whatever is idle by inserting new sources later on...
    const uint64_t later = p.full_refill_cycles;
    for (uint32_t ip = 100000; ip < 100000 + 2048; ++ip) table.find_or_insert(ip, p, later, &st);
    // ...then every original source still maps to exactly one full bucket.
    for (uint32_t ip = 1; ip <= 64; ++ip) {
        PacketTokenBucket* b = table.find_or_insert(ip, p, later, &st);
        if (b == nullptr) continue;   // table legitimately full of active sources
        EXPECT_EQ(b->credits, p.capacity_credits);
        LookupStatus again;
        EXPECT_EQ(table.find_or_insert(ip, p, later, &again), b);
        EXPECT_EQ(again, LookupStatus::kFound);
    }
}
