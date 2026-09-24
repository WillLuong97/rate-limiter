/**
 * Minimal L2/L3/L4 parsing for the Fastpath hot path.
 *
 * Finds the IPv4 header of an Ethernet frame (looking through up to two VLAN
 * tags, 802.1Q / 802.1ad), validates it, and then validates the L4 header for
 * TCP, UDP and ICMP so truncated or bogus packets are dropped instead of being
 * charged to (or slipping past) a source's bucket.
 *
 * Works on raw bytes rather than DPDK structs so it can be unit tested without
 * DPDK. In the app, pass rte_pktmbuf_mtod(m, const uint8_t*) and
 * rte_pktmbuf_data_len(m); only the first segment is inspected, which always
 * holds the headers for the PMDs Fastpath targets.
 * **/
#pragma once

#include <cstdint>
#include <cstring>

namespace aegis::fastpath {

namespace wire {
constexpr uint16_t kEtherTypeIpv4 = 0x0800;
constexpr uint16_t kEtherTypeVlan = 0x8100;
constexpr uint16_t kEtherTypeQinQ = 0x88A8;
constexpr uint32_t kEtherHdrLen = 14;
constexpr uint32_t kVlanHdrLen = 4;
constexpr uint32_t kIpv4MinHdrLen = 20;
constexpr uint32_t kIpv4SrcOffset = 12;
constexpr uint16_t kIpv4FragOffsetMask = 0x1fff;
constexpr uint8_t kProtoIcmp = 1;
constexpr uint8_t kProtoTcp = 6;
constexpr uint8_t kProtoUdp = 17;
constexpr uint32_t kTcpMinHdrLen = 20;
constexpr uint32_t kUdpHdrLen = 8;
constexpr uint32_t kIcmpMinHdrLen = 8;
}  // namespace wire

inline uint16_t read_be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

enum class ParseResult : uint8_t {
    kIpv4,       // PacketInfo is valid
    kNotIpv4,    // ARP, IPv6, LLDP, ... passed through untouched in the MVP
    kMalformed,  // truncated, or claims to be IPv4/TCP/UDP/ICMP but is not; dropped
};

struct PacketInfo {
    uint32_t src_ip_be = 0;   // network byte order, as on the wire
    uint16_t src_port = 0;    // host order; 0 when there is no L4 header to read
    uint16_t dst_port = 0;
    uint8_t proto = 0;        // IP protocol number
    bool non_first_fragment = false;   // carries no L4 header
};

// Frames that claim to be IPv4 but fail validation are reported as kMalformed
// rather than kNotIpv4 so they cannot be used to slip past the limiter.
inline ParseResult parse_packet(const uint8_t* frame, uint32_t len, PacketInfo* info) {
    if (len < wire::kEtherHdrLen) {
        return ParseResult::kMalformed;
    }
    uint32_t off = wire::kEtherHdrLen;
    uint16_t ether_type = read_be16(frame + 12);

    for (int tags = 0; tags < 2 &&
         (ether_type == wire::kEtherTypeVlan || ether_type == wire::kEtherTypeQinQ); ++tags) {
        if (len < off + wire::kVlanHdrLen) {
            return ParseResult::kMalformed;
        }
        ether_type = read_be16(frame + off + 2);
        off += wire::kVlanHdrLen;
    }

    // ── L3 ────────────────────────────────────────────────────────────────────
    if (ether_type != wire::kEtherTypeIpv4) {
        return ParseResult::kNotIpv4;
    }
    if (len < off + wire::kIpv4MinHdrLen) {
        return ParseResult::kMalformed;
    }
    const uint8_t* ip = frame + off;
    const uint32_t ihl = static_cast<uint32_t>(ip[0] & 0x0f) * 4;
    const uint16_t total_len = read_be16(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < wire::kIpv4MinHdrLen || total_len < ihl ||
        len < off + ihl) {
        return ParseResult::kMalformed;
    }
    std::memcpy(&info->src_ip_be, ip + wire::kIpv4SrcOffset, sizeof(info->src_ip_be));
    info->proto = ip[9];
    info->non_first_fragment = (read_be16(ip + 6) & wire::kIpv4FragOffsetMask) != 0;
    info->src_port = 0;
    info->dst_port = 0;

    // ── L4 ────────────────────────────────────────────────────────────────────
    // Later fragments have no L4 header; they are still charged to the source.
    if (info->non_first_fragment) {
        return ParseResult::kIpv4;
    }
    const uint8_t* l4 = ip + ihl;
    // Bytes of L4 present, bounded by both the frame and the IP total length
    // (Ethernet padding on short frames must not count as L4 payload).
    const uint32_t in_frame = len - off - ihl;
    const uint32_t in_ip = total_len - ihl;
    const uint32_t l4_len = in_frame < in_ip ? in_frame : in_ip;

    switch (info->proto) {
        case wire::kProtoTcp: {
            if (l4_len < wire::kTcpMinHdrLen) {
                return ParseResult::kMalformed;
            }
            const uint32_t data_off = static_cast<uint32_t>(l4[12] >> 4) * 4;
            if (data_off < wire::kTcpMinHdrLen || data_off > l4_len) {
                return ParseResult::kMalformed;
            }
            info->src_port = read_be16(l4);
            info->dst_port = read_be16(l4 + 2);
            break;
        }
        case wire::kProtoUdp:
            if (l4_len < wire::kUdpHdrLen) {
                return ParseResult::kMalformed;
            }
            info->src_port = read_be16(l4);
            info->dst_port = read_be16(l4 + 2);
            break;
        case wire::kProtoIcmp:
            if (l4_len < wire::kIcmpMinHdrLen) {
                return ParseResult::kMalformed;
            }
            break;
        default:
            // GRE, ESP, SCTP, ...: not inspected, still limited per source.
            break;
    }
    return ParseResult::kIpv4;
}

}  // namespace aegis::fastpath
