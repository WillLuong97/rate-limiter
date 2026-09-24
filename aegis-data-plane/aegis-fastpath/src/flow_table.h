/**
 * Per-core flow table mapping an IPv4 source address to its token bucket.
 *
 * Each worker core owns exactly one FlowTable and is the only thread that ever
 * touches it (RSS pins a source address to one RX queue, and one RX queue to
 * one core). That is what lets the table skip locks and atomics entirely.
 *
 * Design:
 *  - Fixed-size open addressing with linear probing over a caller-provided
 *    array, so the DPDK app can place it in hugepage memory on the core's
 *    NUMA node and tests can use a std::vector.
 *  - Slots are 32 bytes and 32-byte aligned, two per cache line, so a slot
 *    never straddles lines and one prefetch covers it.
 *  - Probes are capped at kMaxProbe slots so the worst-case cost per packet is
 *    bounded even when the table is nearly full or under a collision attack.
 *  - Entries are never deleted. Instead, a slot whose bucket has been idle
 *    long enough to be full again is recycled for a new source (see
 *    PacketTokenBucket::is_idle). Because nothing is ever emptied, "stop at the
 *    first empty slot" remains a valid lookup rule.
 *  - When the probe window holds neither the key, an empty slot, nor an idle
 *    slot, the lookup returns kFull and the caller applies its overflow policy.
 *
 * Lookups are split into home_index() + prefetch() + find_or_insert() so the
 * caller can issue the prefetches for a whole burst before touching any slot;
 * with millions of sources the table is far bigger than L2, and hiding that
 * DRAM miss is what keeps the per-packet cost in the low hundreds of cycles.
 *
 * Plain C++17, no DPDK dependency.
 * **/
#pragma once

#include <cstddef>
#include <cstdint>

#include "packet_token_bucket.h"

namespace aegis::fastpath {

struct alignas(32) FlowSlot {
    uint32_t src_ip = 0;     // network byte order, as read off the wire
    uint32_t in_use = 0;
    PacketTokenBucket bucket;
};
static_assert(sizeof(FlowSlot) == 32, "two slots per 64-byte cache line");

enum class LookupStatus : uint8_t {
    kFound,     // existing entry for this source
    kInserted,  // new entry in a never-used slot
    kRecycled,  // new entry that replaced an idle source
    kFull,      // no room within the probe window; no bucket returned
};

class FlowTable {
public:
    static constexpr uint32_t kMaxProbe = 16;

    // `slots` must hold `capacity` zero-initialised entries and `capacity`
    // must be a power of two. The table does not own the memory.
    FlowTable(FlowSlot* slots, uint32_t capacity, uint64_t seed)
        : slots_(slots), mask_(capacity - 1), seed_(seed) {}

    static bool valid_capacity(uint64_t capacity) {
        return capacity >= kMaxProbe && capacity <= (1ULL << 31) &&
               (capacity & (capacity - 1)) == 0;
    }

    uint32_t home_index(uint32_t src_ip) const { return hash(src_ip) & mask_; }

    // Most lookups resolve in the home slot or its neighbour, which share a
    // cache line, so one prefetch is enough in the common case.
    void prefetch(uint32_t home) const { __builtin_prefetch(&slots_[home], 1, 3); }

    PacketTokenBucket* find_or_insert(uint32_t src_ip, const BucketParams& p,
                                      uint64_t now, LookupStatus* status) {
        return find_or_insert_at(home_index(src_ip), src_ip, p, now, status);
    }

    // `home` must be home_index(src_ip).
    PacketTokenBucket* find_or_insert_at(uint32_t home, uint32_t src_ip, const BucketParams& p,
                                         uint64_t now, LookupStatus* status) {
        uint32_t idx = home;
        FlowSlot* recyclable = nullptr;

        for (uint32_t i = 0; i < kMaxProbe; ++i, idx = (idx + 1) & mask_) {
            FlowSlot& s = slots_[idx];
            if (!s.in_use) {
                // First empty slot ends the chain: the key is not present.
                // Prefer an idle slot seen earlier so chains stay short.
                if (recyclable != nullptr) {
                    return claim(*recyclable, src_ip, p, now, LookupStatus::kRecycled, status);
                }
                return claim(s, src_ip, p, now, LookupStatus::kInserted, status);
            }
            if (s.src_ip == src_ip) {
                *status = LookupStatus::kFound;
                return &s.bucket;
            }
            if (recyclable == nullptr && s.bucket.is_idle(p, now)) {
                recyclable = &s;
            }
        }

        if (recyclable != nullptr) {
            return claim(*recyclable, src_ip, p, now, LookupStatus::kRecycled, status);
        }
        *status = LookupStatus::kFull;
        return nullptr;
    }

    uint32_t capacity() const { return mask_ + 1; }

private:
    PacketTokenBucket* claim(FlowSlot& s, uint32_t src_ip, const BucketParams& p,
                             uint64_t now, LookupStatus result, LookupStatus* status) {
        s.src_ip = src_ip;
        s.in_use = 1;
        s.bucket.init(p, now);
        *status = result;
        return &s.bucket;
    }

    // murmur3 64-bit finaliser. Seeded per process so an attacker cannot
    // precompute a set of source addresses that all land in one probe window.
    //
    // Note: do not reuse mbuf->hash.rss here. Its low bits already chose the
    // RX queue, so on any one core they are nearly constant and would pile
    // every source into a small part of the table.
    uint32_t hash(uint32_t key) const {
        uint64_t h = key ^ seed_;
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return static_cast<uint32_t>(h);
    }

    FlowSlot* slots_;
    uint32_t mask_;
    uint64_t seed_;
};

}  // namespace aegis::fastpath
