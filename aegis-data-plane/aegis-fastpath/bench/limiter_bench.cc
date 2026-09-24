/**
 * Single-core micro-benchmark of the Fastpath decision path (parse + flow
 * table + token bucket) on synthetic 64-byte UDP frames, with no NIC or DPDK.
 *
 * It measures the limiter only: RX/TX descriptor handling and mbuf free are not
 * included, so the full app costs more per packet than this reports. Use it to
 * catch regressions in the core logic and to see how the cost grows once the
 * flow table no longer fits in cache.
 *
 * Usage: limiter_bench [sources=1000000] [flows=2097152] [packets=50000000] [ghz=3.0]
 *   ghz is only used to convert ns/packet into an approximate cycles/packet.
 * **/
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "source_limiter.h"

using namespace aegis::fastpath;

namespace {

std::vector<uint8_t> udp_frame(uint32_t src_ip_be) {
    std::vector<uint8_t> f(64, 0);
    f[12] = 0x08;                    // IPv4
    f[14] = 0x45;                    // v4, IHL 5
    f[16] = 0; f[17] = 46;           // total length: 64 - 14 - 4 (FCS)
    f[23] = 17;                      // UDP
    __builtin_memcpy(&f[26], &src_ip_be, 4);
    f[34] = 0x9c; f[35] = 0x40;      // src port 40000
    f[36] = 0x1f; f[37] = 0x90;      // dst port 8080
    return f;
}

uint64_t arg(int argc, char** argv, int i, uint64_t def) {
    return argc > i ? std::strtoull(argv[i], nullptr, 10) : def;
}

}  // namespace

int main(int argc, char** argv) {
    const uint64_t sources = arg(argc, argv, 1, 1'000'000);
    const uint64_t flows = arg(argc, argv, 2, 1 << 21);
    const uint64_t packets = arg(argc, argv, 3, 50'000'000);
    const double ghz = argc > 4 ? std::atof(argv[4]) : 3.0;
    if (!FlowTable::valid_capacity(flows) || sources == 0) {
        std::fprintf(stderr, "flows must be a power of two >= 16, sources > 0\n");
        return 1;
    }

    // One reusable frame per burst slot; each packet gets a fresh random source
    // (xorshift, a few cycles) so the flow table access pattern is as
    // cache-hostile as real traffic from `sources` distinct senders.
    std::vector<std::vector<uint8_t>> frames;
    for (uint32_t i = 0; i < 32; ++i) frames.push_back(udp_frame(0));
    uint64_t rng = 0x9e3779b97f4a7c15ULL;
    auto next_src = [&rng, sources]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return 0x0a000000u + static_cast<uint32_t>(rng % sources);
    };

    // 1 "cycle" == 1 ns; a high rate keeps most packets on the allow path,
    // which is the more expensive one (it goes on to TX in the real app).
    BucketParams p;
    BucketParams::make(1'000'000'000, 1'000'000, 1'000'000, &p);
    std::vector<FlowSlot> slots(flows);
    SourceLimiter limiter(slots.data(), static_cast<uint32_t>(flows), 99, p,
                          TableFullPolicy::kAllow);

    constexpr uint16_t kBurst = 32;
    const uint8_t* ptrs[kBurst];
    uint32_t lens[kBurst];
    Verdict verdicts[kBurst];
    uint64_t forwarded = 0;

    const auto start = std::chrono::steady_clock::now();
    for (uint16_t i = 0; i < kBurst; ++i) {
        ptrs[i] = frames[i].data();
        lens[i] = static_cast<uint32_t>(frames[i].size());
    }
    for (uint64_t done = 0; done < packets; done += kBurst) {
        for (uint16_t i = 0; i < kBurst; ++i) {
            const uint32_t src = next_src();
            __builtin_memcpy(&frames[i][26], &src, 4);
        }
        const uint64_t now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
        limiter.classify_burst(ptrs, lens, kBurst, now, verdicts);
        for (uint16_t i = 0; i < kBurst; ++i) forwarded += verdicts[i] == Verdict::kForward;
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    const double ns_per_pkt = secs * 1e9 / static_cast<double>(packets);
    std::printf("sources=%llu flows=%llu packets=%llu\n", (unsigned long long)sources,
                (unsigned long long)flows, (unsigned long long)packets);
    std::printf("%.2f Mpps  %.2f ns/pkt  ~%.0f cycles/pkt @ %.1f GHz  (forwarded %.1f%%, "
                "table_full %llu)\n",
                packets / secs / 1e6, ns_per_pkt, ns_per_pkt * ghz, ghz,
                100.0 * forwarded / packets,
                (unsigned long long)limiter.counters().table_full.get());
    return 0;
}
