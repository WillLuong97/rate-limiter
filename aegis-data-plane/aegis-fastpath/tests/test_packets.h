// Helpers for building raw Ethernet frames in tests.
#pragma once

#include <cstdint>
#include <vector>

namespace aegis::fastpath::testing {

inline uint32_t ip_be(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    // Bytes in wire order, reinterpreted exactly as the parser's memcpy does.
    const uint8_t bytes[4] = {a, b, c, d};
    uint32_t v;
    __builtin_memcpy(&v, bytes, 4);
    return v;
}

struct FrameSpec {
    uint32_t src_ip_be = 0;
    uint8_t proto = 17;          // UDP
    uint16_t src_port = 40000;
    uint16_t dst_port = 8080;
    int vlan_tags = 0;
    uint16_t frag_offset = 0;    // in 8-byte units
    uint8_t ihl_words = 5;
    uint8_t version = 4;
    uint8_t tcp_data_off_words = 5;
};

// Builds Ethernet [+ VLANs] + IPv4 + minimal L4 header (+ 8 bytes payload).
inline std::vector<uint8_t> make_frame(const FrameSpec& s) {
    std::vector<uint8_t> f(12, 0xaa);   // dst + src MAC
    auto put16 = [&f](uint16_t v) {
        f.push_back(static_cast<uint8_t>(v >> 8));
        f.push_back(static_cast<uint8_t>(v));
    };
    for (int i = 0; i < s.vlan_tags; ++i) {
        put16(i == 0 && s.vlan_tags == 2 ? 0x88A8 : 0x8100);
        put16(100 + i);   // TCI
    }
    put16(0x0800);

    uint32_t l4_len = 8;
    if (s.proto == 6) l4_len = 20;
    const uint32_t ihl = s.ihl_words * 4u;
    const uint16_t total = static_cast<uint16_t>(ihl + l4_len + 8);

    const size_t ip = f.size();
    f.resize(ip + ihl, 0);
    f[ip] = static_cast<uint8_t>((s.version << 4) | s.ihl_words);
    f[ip + 2] = static_cast<uint8_t>(total >> 8);
    f[ip + 3] = static_cast<uint8_t>(total);
    f[ip + 6] = static_cast<uint8_t>((s.frag_offset >> 8) & 0x1f);
    f[ip + 7] = static_cast<uint8_t>(s.frag_offset);
    f[ip + 8] = 64;
    f[ip + 9] = s.proto;
    __builtin_memcpy(&f[ip + 12], &s.src_ip_be, 4);

    const size_t l4 = f.size();
    f.resize(l4 + l4_len + 8, 0);
    f[l4] = static_cast<uint8_t>(s.src_port >> 8);
    f[l4 + 1] = static_cast<uint8_t>(s.src_port);
    f[l4 + 2] = static_cast<uint8_t>(s.dst_port >> 8);
    f[l4 + 3] = static_cast<uint8_t>(s.dst_port);
    if (s.proto == 6) {
        f[l4 + 12] = static_cast<uint8_t>(s.tcp_data_off_words << 4);
    }
    return f;
}

}  // namespace aegis::fastpath::testing
