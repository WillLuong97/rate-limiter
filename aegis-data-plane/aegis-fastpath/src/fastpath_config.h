/**
 * Application options for Aegis Fastpath (everything after the EAL's `--`).
 *
 * Parsing is kept free of DPDK so it can be unit tested on any machine.
 * **/
#pragma once

#include <cstdint>
#include <string>

namespace aegis::fastpath {

enum class TableFullPolicy : uint8_t {
    kAllow,   // fail open, matches Aegis Engine's behaviour when Redis is down
    kDrop,    // fail closed
};

struct FastpathConfig {
    uint64_t rate_pps = 1000;          // sustained packets/sec allowed per source IP
    uint64_t burst = 2000;             // bucket depth in packets
    uint64_t flows_per_core = 1 << 20; // flow table slots per worker (power of two)
    TableFullPolicy on_table_full = TableFullPolicy::kAllow;
    uint32_t stats_interval_s = 1;     // 0 disables periodic stats
};

// Parses app arguments (argv[0] is the program name and is skipped).
// Returns true on success; on failure fills `error` with a human-readable message.
bool parse_fastpath_args(int argc, char** argv, FastpathConfig* cfg, std::string* error);

std::string fastpath_usage(const char* prog);

}  // namespace aegis::fastpath
