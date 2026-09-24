/**
 * Per-core packet admission decision for Aegis Fastpath:
 * parse (L2/L3/L4) -> flow table lookup -> token bucket -> verdict.
 *
 * One SourceLimiter lives on each worker core and is written only by that
 * core, so there are no locks. The main core reads the counters for stats,
 * which is why they are relaxed atomics (a plain load/store on x86-64 and
 * arm64, no lock prefix).
 *
 * classify_burst() is the hot path. It runs in two passes over an RX burst:
 *   1. parse every packet, hash its source, and prefetch its flow slot;
 *   2. look up / charge each bucket, by which time the slots are in cache.
 * With a large table this overlaps up to a burst's worth of DRAM misses
 * instead of paying them one after another.
 *
 * Plain C++17, no DPDK dependency, so the full decision path is unit tested.
 * **/
#pragma once

#include <atomic>
#include <cstdint>

#include "fastpath_config.h"
#include "flow_table.h"
#include "packet_parse.h"
#include "packet_token_bucket.h"

namespace aegis::fastpath {

enum class Verdict : uint8_t {
    kForward,
    kDrop,
};

// Single-writer counter: the owning core increments, anyone may read.
class Counter {
public:
    void add(uint64_t n = 1) {
        value_.store(value_.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }
    uint64_t get() const { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> value_{0};
};

struct LimiterCounters {
    Counter allowed;            // IPv4, had a token
    Counter rate_limited;       // IPv4, bucket empty -> dropped
    Counter non_ipv4;           // passed through untouched
    Counter malformed;          // bad L3/L4 header -> dropped
    Counter table_full;         // no bucket available, handled by TableFullPolicy
    Counter new_sources;        // bucket created in an empty slot
    Counter recycled_sources;   // bucket created by reclaiming an idle slot
    Counter tcp, udp, icmp, other_l4, fragments;   // IPv4 traffic mix
};

class SourceLimiter {
public:
    static constexpr uint16_t kMaxBurst = 64;

    SourceLimiter(FlowSlot* slots, uint32_t capacity, uint64_t seed,
                  const BucketParams& params, TableFullPolicy policy)
        : table_(slots, capacity, seed), params_(params), policy_(policy) {}

    // Classifies `n` (<= kMaxBurst) frames that all arrived at time `now`.
    void classify_burst(const uint8_t* const* frames, const uint32_t* lens, uint16_t n,
                        uint64_t now, Verdict* verdicts) {
        ParseResult parsed[kMaxBurst];
        PacketInfo info[kMaxBurst];
        uint32_t home[kMaxBurst];

        // Pass 1: parse + prefetch.
        for (uint16_t i = 0; i < n; ++i) {
            parsed[i] = parse_packet(frames[i], lens[i], &info[i]);
            if (parsed[i] == ParseResult::kIpv4) {
                home[i] = table_.home_index(info[i].src_ip_be);
                table_.prefetch(home[i]);
            }
        }

        // Pass 2: decide.
        uint64_t tcp = 0, udp = 0, icmp = 0, other = 0, frags = 0;
        for (uint16_t i = 0; i < n; ++i) {
            switch (parsed[i]) {
                case ParseResult::kNotIpv4:
                    counters_.non_ipv4.add();
                    verdicts[i] = Verdict::kForward;
                    continue;
                case ParseResult::kMalformed:
                    counters_.malformed.add();
                    verdicts[i] = Verdict::kDrop;
                    continue;
                case ParseResult::kIpv4:
                    break;
            }
            frags += info[i].non_first_fragment;
            switch (info[i].proto) {
                case wire::kProtoTcp: ++tcp; break;
                case wire::kProtoUdp: ++udp; break;
                case wire::kProtoIcmp: ++icmp; break;
                default: ++other; break;
            }
            verdicts[i] = decide(home[i], info[i].src_ip_be, now);
        }
        counters_.tcp.add(tcp);
        counters_.udp.add(udp);
        counters_.icmp.add(icmp);
        counters_.other_l4.add(other);
        counters_.fragments.add(frags);
    }

    // Single-packet convenience wrapper, mainly for tests.
    Verdict classify(const uint8_t* frame, uint32_t len, uint64_t now) {
        Verdict v;
        classify_burst(&frame, &len, 1, now, &v);
        return v;
    }

    const LimiterCounters& counters() const { return counters_; }

private:
    Verdict decide(uint32_t home, uint32_t src_ip, uint64_t now) {
        LookupStatus status;
        PacketTokenBucket* bucket = table_.find_or_insert_at(home, src_ip, params_, now, &status);
        switch (status) {
            case LookupStatus::kFound:
                break;
            case LookupStatus::kInserted:
                counters_.new_sources.add();
                break;
            case LookupStatus::kRecycled:
                counters_.recycled_sources.add();
                break;
            case LookupStatus::kFull:
                counters_.table_full.add();
                return policy_ == TableFullPolicy::kAllow ? Verdict::kForward : Verdict::kDrop;
        }
        if (bucket->consume(params_, now)) {
            counters_.allowed.add();
            return Verdict::kForward;
        }
        counters_.rate_limited.add();
        return Verdict::kDrop;
    }

    FlowTable table_;
    BucketParams params_;
    TableFullPolicy policy_;
    LimiterCounters counters_;
};

}  // namespace aegis::fastpath
