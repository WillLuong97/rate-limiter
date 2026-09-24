#include "source_limiter.h"

#include <gtest/gtest.h>

#include <vector>

#include "test_packets.h"

using namespace aegis::fastpath;
using namespace aegis::fastpath::testing;

namespace {

constexpr uint64_t kHz = 1'000'000'000;

struct LimiterFixture : ::testing::Test {
    LimiterFixture() : slots(1024) {}

    SourceLimiter make(uint64_t rate, uint64_t burst,
                       TableFullPolicy policy = TableFullPolicy::kAllow) {
        BucketParams p;
        EXPECT_TRUE(BucketParams::make(kHz, rate, burst, &p));
        return SourceLimiter(slots.data(), static_cast<uint32_t>(slots.size()), 7, p, policy);
    }

    static std::vector<uint8_t> udp_from(uint32_t src) {
        FrameSpec s;
        s.src_ip_be = src;
        return make_frame(s);
    }

    static Verdict send(SourceLimiter& l, const std::vector<uint8_t>& f, uint64_t now) {
        return l.classify(f.data(), static_cast<uint32_t>(f.size()), now);
    }

    std::vector<FlowSlot> slots;
};

}  // namespace

TEST_F(LimiterFixture, EachSourceHasItsOwnBudget) {
    SourceLimiter l = make(10, 3);
    const auto a = udp_from(ip_be(1, 1, 1, 1));
    const auto b = udp_from(ip_be(2, 2, 2, 2));

    for (int i = 0; i < 3; ++i) EXPECT_EQ(send(l, a, 0), Verdict::kForward);
    EXPECT_EQ(send(l, a, 0), Verdict::kDrop);
    // A heavy hitter must not eat anyone else's budget.
    EXPECT_EQ(send(l, b, 0), Verdict::kForward);

    EXPECT_EQ(l.counters().allowed.get(), 4u);
    EXPECT_EQ(l.counters().rate_limited.get(), 1u);
    EXPECT_EQ(l.counters().new_sources.get(), 2u);
}

TEST_F(LimiterFixture, AllProtocolsShareTheSourceBudget) {
    SourceLimiter l = make(10, 2);
    FrameSpec tcp;
    tcp.src_ip_be = ip_be(9, 9, 9, 9);
    tcp.proto = wire::kProtoTcp;
    FrameSpec icmp = tcp;
    icmp.proto = wire::kProtoIcmp;
    FrameSpec udp = tcp;
    udp.proto = wire::kProtoUdp;

    EXPECT_EQ(send(l, make_frame(tcp), 0), Verdict::kForward);
    EXPECT_EQ(send(l, make_frame(icmp), 0), Verdict::kForward);
    EXPECT_EQ(send(l, make_frame(udp), 0), Verdict::kDrop);
    EXPECT_EQ(l.counters().tcp.get(), 1u);
    EXPECT_EQ(l.counters().icmp.get(), 1u);
    EXPECT_EQ(l.counters().udp.get(), 1u);
}

TEST_F(LimiterFixture, NonIpv4ForwardedMalformedDropped) {
    SourceLimiter l = make(1, 1);
    std::vector<uint8_t> arp(60, 0);
    arp[12] = 0x08;
    arp[13] = 0x06;
    for (int i = 0; i < 10; ++i) EXPECT_EQ(send(l, arp, 0), Verdict::kForward);

    FrameSpec bad;
    bad.version = 5;
    EXPECT_EQ(send(l, make_frame(bad), 0), Verdict::kDrop);
    EXPECT_EQ(l.counters().non_ipv4.get(), 10u);
    EXPECT_EQ(l.counters().malformed.get(), 1u);
}

TEST_F(LimiterFixture, BurstMatchesPerPacketDecisions) {
    SourceLimiter burst = make(10, 4);
    std::vector<FlowSlot> other_slots(1024);
    BucketParams p;
    BucketParams::make(kHz, 10, 4, &p);
    SourceLimiter single(other_slots.data(), 1024, 7, p, TableFullPolicy::kAllow);

    std::vector<std::vector<uint8_t>> frames;
    for (int i = 0; i < 32; ++i) frames.push_back(udp_from(ip_be(10, 0, 0, i % 5)));
    const uint8_t* ptrs[32];
    uint32_t lens[32];
    for (int i = 0; i < 32; ++i) {
        ptrs[i] = frames[i].data();
        lens[i] = static_cast<uint32_t>(frames[i].size());
    }
    Verdict v[32];
    burst.classify_burst(ptrs, lens, 32, 0, v);
    for (int i = 0; i < 32; ++i) {
        EXPECT_EQ(v[i], single.classify(ptrs[i], lens[i], 0)) << i;
    }
    EXPECT_EQ(burst.counters().allowed.get(), 5u * 4u);   // 5 sources x burst 4
}

TEST_F(LimiterFixture, TableFullPolicy) {
    slots.assign(16, FlowSlot{});
    SourceLimiter allow = make(10, 10, TableFullPolicy::kAllow);
    for (uint32_t i = 1; i <= 16; ++i) send(allow, udp_from(i), 0);
    EXPECT_EQ(send(allow, udp_from(999), 0), Verdict::kForward);
    EXPECT_EQ(allow.counters().table_full.get(), 1u);

    slots.assign(16, FlowSlot{});
    SourceLimiter drop = make(10, 10, TableFullPolicy::kDrop);
    for (uint32_t i = 1; i <= 16; ++i) send(drop, udp_from(i), 0);
    EXPECT_EQ(send(drop, udp_from(999), 0), Verdict::kDrop);
}
