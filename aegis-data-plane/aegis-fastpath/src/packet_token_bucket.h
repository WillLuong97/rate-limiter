/**
 * Per-source packet token bucket used by Aegis Fastpath.
 *
 * This is the line-rate counterpart of rate-limiting-algorithm/token_bucket.h in
 * Aegis Engine. The Engine version keeps its state in Redis so every server sees
 * the same bucket; that costs a network round trip per request, which is fine
 * for L7 requests but far too slow for packets. Fastpath instead keeps every
 * bucket in the memory of the single core that owns the source (see
 * flow_table.h and the RSS setup in fastpath_main.cc), so there is no locking and no
 * shared state, and the whole check is a few integer instructions.
 *
 * Time is measured in CPU cycles (rte_rdtsc() in the app, any monotonic counter
 * in tests) and tokens are stored in fixed point: one packet costs `hz` credits
 * and the bucket refills `rate_pps` credits per cycle. Using integers avoids
 * floating point on the hot path and makes the refill exact.
 *
 * This header is plain C++17 with no DPDK dependency so it can be unit tested on
 * any machine.
 * **/
#pragma once

#include <cstdint>

namespace aegis::fastpath {

// Static configuration shared by every bucket on a core.
struct BucketParams {
    uint64_t hz = 0;                  // counter ticks per second (rte_get_tsc_hz())
    uint64_t rate_pps = 0;            // sustained packets per second per source
    uint64_t burst = 0;               // max packets a source can send back to back
    uint64_t capacity_credits = 0;    // burst * hz
    uint64_t full_refill_cycles = 0;  // cycles for an empty bucket to become full

    // Returns false if the combination would overflow the fixed-point math.
    static bool make(uint64_t hz, uint64_t rate_pps, uint64_t burst, BucketParams* out) {
        if (hz == 0 || rate_pps == 0 || burst == 0) {
            return false;
        }
        // capacity_credits = burst * hz must fit in 64 bits.
        if (burst > UINT64_MAX / hz) {
            return false;
        }
        out->hz = hz;
        out->rate_pps = rate_pps;
        out->burst = burst;
        out->capacity_credits = burst * hz;
        // Round up so that after full_refill_cycles the bucket is guaranteed full.
        out->full_refill_cycles = (out->capacity_credits + rate_pps - 1) / rate_pps;
        return true;
    }
};

// 16 bytes per source. Kept deliberately small so a million-entry flow table
// still fits in a few tens of MB per core.
struct PacketTokenBucket {
    uint64_t credits = 0;     // available tokens, scaled by params.hz
    uint64_t last_cycles = 0; // counter value at the last refill

    void init(const BucketParams& p, uint64_t now) {
        credits = p.capacity_credits;   // new sources start with a full burst
        last_cycles = now;
    }

    // Refill for the time elapsed since the last call, then try to take one
    // packet's worth of credit. Returns true if the packet is allowed.
    bool consume(const BucketParams& p, uint64_t now) {
        refill(p, now);
        if (credits >= p.hz) {
            credits -= p.hz;
            return true;
        }
        return false;
    }

    void refill(const BucketParams& p, uint64_t now) {
        // Counters only move forward; a stale `now` (for example when two
        // bursts are timestamped out of order) must not underflow.
        if (now <= last_cycles) {
            return;
        }
        const uint64_t elapsed = now - last_cycles;
        last_cycles = now;

        // Clamp before multiplying: elapsed * rate_pps can overflow after long
        // idle periods, but anything past full_refill_cycles means "full" anyway.
        if (elapsed >= p.full_refill_cycles) {
            credits = p.capacity_credits;
            return;
        }
        const uint64_t added = elapsed * p.rate_pps;   // < capacity_credits, cannot overflow
        const uint64_t room = p.capacity_credits - credits;
        credits = added >= room ? p.capacity_credits : credits + added;
    }

    // A bucket untouched for full_refill_cycles is full, which is exactly the
    // state a brand-new bucket starts in. Such an entry carries no information
    // and its flow table slot can be recycled without changing any decision.
    bool is_idle(const BucketParams& p, uint64_t now) const {
        return now > last_cycles && now - last_cycles >= p.full_refill_cycles;
    }
};

}  // namespace aegis::fastpath
