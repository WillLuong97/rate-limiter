#include "fastpath_config.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "flow_table.h"

namespace aegis::fastpath {

namespace {

bool parse_u64(const char* s, uint64_t* out) {
    if (s == nullptr || *s == '\0' || *s == '-') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 10);
    if (errno != 0 || *end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

}  // namespace

bool parse_fastpath_args(int argc, char** argv, FastpathConfig* cfg, std::string* error) {
    for (int i = 1; i < argc; ++i) {
        const std::string opt = argv[i];
        const char* val = (i + 1 < argc) ? argv[i + 1] : nullptr;
        uint64_t n = 0;

        if (opt == "--rate" || opt == "--burst" || opt == "--flows" ||
            opt == "--stats-interval") {
            if (!parse_u64(val, &n)) {
                *error = opt + " expects a non-negative integer";
                return false;
            }
            ++i;
            if (opt == "--rate") {
                cfg->rate_pps = n;
            } else if (opt == "--burst") {
                cfg->burst = n;
            } else if (opt == "--flows") {
                cfg->flows_per_core = n;
            } else {
                cfg->stats_interval_s = static_cast<uint32_t>(n);
            }
        } else if (opt == "--on-table-full") {
            if (val != nullptr && std::strcmp(val, "allow") == 0) {
                cfg->on_table_full = TableFullPolicy::kAllow;
            } else if (val != nullptr && std::strcmp(val, "drop") == 0) {
                cfg->on_table_full = TableFullPolicy::kDrop;
            } else {
                *error = "--on-table-full expects 'allow' or 'drop'";
                return false;
            }
            ++i;
        } else {
            *error = "unknown option: " + opt;
            return false;
        }
    }

    if (cfg->rate_pps == 0) {
        *error = "--rate must be greater than 0";
        return false;
    }
    if (cfg->burst == 0) {
        *error = "--burst must be greater than 0";
        return false;
    }
    if (!FlowTable::valid_capacity(cfg->flows_per_core)) {
        *error = "--flows must be a power of two between 16 and 2^31";
        return false;
    }
    return true;
}

std::string fastpath_usage(const char* prog) {
    return std::string("Usage: ") + prog + " [EAL options] -- [app options]\n"
        "  --rate <pps>              sustained packets/sec per source IPv4 (default 1000)\n"
        "  --burst <packets>         bucket depth per source (default 2000)\n"
        "  --flows <n>               flow table slots per worker core, power of two (default 1048576)\n"
        "  --on-table-full allow|drop  policy when a core's flow table is full (default allow)\n"
        "  --stats-interval <sec>    print counters every N seconds, 0 to disable (default 1)\n";
}

}  // namespace aegis::fastpath
