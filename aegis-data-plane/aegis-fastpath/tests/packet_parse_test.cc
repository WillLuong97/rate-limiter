#include "packet_parse.h"

#include <gtest/gtest.h>

#include "test_packets.h"

using namespace aegis::fastpath;
using namespace aegis::fastpath::testing;

namespace {

ParseResult parse(const std::vector<uint8_t>& f, PacketInfo* info) {
    return parse_packet(f.data(), static_cast<uint32_t>(f.size()), info);
}

}  // namespace

TEST(PacketParse, UdpFrame) {
    FrameSpec s;
    s.src_ip_be = ip_be(203, 0, 113, 42);
    PacketInfo info;
    ASSERT_EQ(parse(make_frame(s), &info), ParseResult::kIpv4);
    EXPECT_EQ(info.src_ip_be, ip_be(203, 0, 113, 42));
    EXPECT_EQ(info.proto, wire::kProtoUdp);
    EXPECT_EQ(info.src_port, 40000);
    EXPECT_EQ(info.dst_port, 8080);
    EXPECT_FALSE(info.non_first_fragment);
}

TEST(PacketParse, TcpFrame) {
    FrameSpec s;
    s.proto = wire::kProtoTcp;
    s.dst_port = 443;
    PacketInfo info;
    ASSERT_EQ(parse(make_frame(s), &info), ParseResult::kIpv4);
    EXPECT_EQ(info.proto, wire::kProtoTcp);
    EXPECT_EQ(info.dst_port, 443);
}

TEST(PacketParse, IcmpFrame) {
    FrameSpec s;
    s.proto = wire::kProtoIcmp;
    PacketInfo info;
    ASSERT_EQ(parse(make_frame(s), &info), ParseResult::kIpv4);
    EXPECT_EQ(info.src_port, 0);
}

TEST(PacketParse, SingleAndDoubleVlan) {
    for (int tags : {1, 2}) {
        FrameSpec s;
        s.vlan_tags = tags;
        s.src_ip_be = ip_be(10, 0, 0, tags);
        PacketInfo info;
        ASSERT_EQ(parse(make_frame(s), &info), ParseResult::kIpv4) << tags;
        EXPECT_EQ(info.src_ip_be, ip_be(10, 0, 0, tags));
    }
}

TEST(PacketParse, IpOptionsAreSkipped) {
    FrameSpec s;
    s.ihl_words = 6;
    PacketInfo info;
    ASSERT_EQ(parse(make_frame(s), &info), ParseResult::kIpv4);
    EXPECT_EQ(info.dst_port, 8080);
}

TEST(PacketParse, NonFirstFragmentHasNoPorts) {
    FrameSpec s;
    s.frag_offset = 185;
    PacketInfo info;
    ASSERT_EQ(parse(make_frame(s), &info), ParseResult::kIpv4);
    EXPECT_TRUE(info.non_first_fragment);
    EXPECT_EQ(info.dst_port, 0);
}

TEST(PacketParse, NonIpv4PassesThrough) {
    std::vector<uint8_t> arp(60, 0);
    arp[12] = 0x08;
    arp[13] = 0x06;
    PacketInfo info;
    EXPECT_EQ(parse(arp, &info), ParseResult::kNotIpv4);
}

TEST(PacketParse, MalformedIsDropped) {
    PacketInfo info;
    EXPECT_EQ(parse(std::vector<uint8_t>(10, 0), &info), ParseResult::kMalformed);

    FrameSpec bad_version;
    bad_version.version = 6;
    EXPECT_EQ(parse(make_frame(bad_version), &info), ParseResult::kMalformed);

    FrameSpec bad_ihl;
    bad_ihl.ihl_words = 4;
    EXPECT_EQ(parse(make_frame(bad_ihl), &info), ParseResult::kMalformed);

    FrameSpec bad_tcp;
    bad_tcp.proto = wire::kProtoTcp;
    bad_tcp.tcp_data_off_words = 2;
    EXPECT_EQ(parse(make_frame(bad_tcp), &info), ParseResult::kMalformed);

    // Truncated in the middle of the UDP header.
    FrameSpec udp;
    std::vector<uint8_t> f = make_frame(udp);
    f.resize(14 + 20 + 4);
    EXPECT_EQ(parse(f, &info), ParseResult::kMalformed);

    // Truncated inside a VLAN tag.
    FrameSpec vlan;
    vlan.vlan_tags = 1;
    f = make_frame(vlan);
    f.resize(16);
    EXPECT_EQ(parse(f, &info), ParseResult::kMalformed);
}
