/**
 * Aegis Fastpath: DPDK packet-rate enforcement in front of Aegis Engine.
 *
 * Aegis Engine limits L7 requests per client IP through Envoy + Redis. Fastpath
 * is the second, lower layer: it sits on the wire in front of the gateway,
 * validates L3/L4 headers, and drops packets from any IPv4 source that exceeds
 * a packets-per-second budget, before they cost the kernel, Envoy, or Redis
 * anything.
 *
 * Reference deployment: 4 worker lcores (EAL -l 0-4: lcore 0 = main/stats,
 * lcores 1-4 = workers), each pinned by EAL to its own isolated CPU.
 *
 * Threading model (run-to-completion, shared-nothing):
 *
 *   NIC RSS (hash on IPv4 source)
 *        |-- RX queue 0 --> worker lcore A --> own FlowTable --> TX queue 0
 *        |-- RX queue 1 --> worker lcore B --> own FlowTable --> TX queue 1
 *        '-- ...
 *   main lcore: prints stats, handles SIGINT/SIGTERM
 *
 * Every worker is an EAL lcore, which DPDK pins to one CPU. Because RSS sends
 * all packets from a source to the same queue, each source's bucket lives on
 * exactly one core, so the hot path has no locks and no shared cache lines.
 *
 * Ports:
 *   1 port : packets are limited and sent back out the same port (lab/demo).
 *   2 ports: bump in the wire. Port 0 faces clients and is limited; traffic
 *            arriving on port 1 (responses from the protected side) is
 *            forwarded to port 0 untouched.
 * **/

#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_prefetch.h>
#include <rte_random.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "fastpath_config.h"
#include "packet_token_bucket.h"
#include "source_limiter.h"

using namespace aegis::fastpath;

namespace {

constexpr uint16_t kRxDesc = 1024;
constexpr uint16_t kTxDesc = 1024;
constexpr uint16_t kBurst = 32;
constexpr unsigned kMbufCache = 256;
constexpr uint16_t kMaxPorts = 2;
constexpr unsigned kRecommendedWorkers = 4;

std::atomic<bool> g_force_quit{false};

void on_signal(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        g_force_quit.store(true, std::memory_order_relaxed);
    }
}

struct PortCounters {
    Counter tx_failed;   // TX ring full, packet freed
    Counter passthrough; // arrived on the inside port, forwarded unlimited
};

// Hot-path cost accounting. Only bursts that carried packets are timed, so
// busy_cycles / packets is the true per-packet cost (RX + parse + lookup +
// bucket + TX/free), independent of how much time the core spends idle-polling.
struct PerfCounters {
    Counter packets;       // received on the limited (outside) port
    Counter busy_cycles;   // TSC cycles spent processing those bursts
};

struct alignas(RTE_CACHE_LINE_SIZE) WorkerContext {
    unsigned lcore_id = 0;
    uint16_t queue_id = 0;
    uint16_t outside_port = 0;   // limited direction
    uint16_t inside_port = 0;    // == outside_port in single-port mode
    bool two_ports = false;
    FlowSlot* slots = nullptr;
    SourceLimiter* limiter = nullptr;
    PortCounters port_counters;
    PerfCounters perf;
};

// Sends `n` packets and frees whatever the TX ring could not take.
inline void send_burst(uint16_t port, uint16_t queue, rte_mbuf** pkts, uint16_t n,
                       Counter& tx_failed) {
    if (n == 0) {
        return;
    }
    const uint16_t sent = rte_eth_tx_burst(port, queue, pkts, n);
    if (unlikely(sent < n)) {
        tx_failed.add(n - sent);
        rte_pktmbuf_free_bulk(pkts + sent, n - sent);
    }
}

int worker_main(void* arg) {
    auto* ctx = static_cast<WorkerContext*>(arg);
    SourceLimiter& limiter = *ctx->limiter;
    rte_mbuf* rx[kBurst];
    rte_mbuf* fwd[kBurst];
    rte_mbuf* drop[kBurst];
    const uint8_t* frames[kBurst];
    uint32_t lens[kBurst];
    Verdict verdicts[kBurst];
    static_assert(kBurst <= SourceLimiter::kMaxBurst, "burst larger than limiter batch");

    printf("[fastpath] worker lcore %u (socket %u) -> queue %u\n", ctx->lcore_id,
           rte_lcore_to_socket_id(ctx->lcore_id), ctx->queue_id);

    while (!g_force_quit.load(std::memory_order_relaxed)) {
        // ── Outside -> inside: enforce per-source rate ────────────────────────
        const uint16_t nb_rx = rte_eth_rx_burst(ctx->outside_port, ctx->queue_id, rx, kBurst);
        if (nb_rx > 0) {
            // One timestamp per burst: a burst is received in well under a
            // microsecond, far below the resolution any realistic rate needs.
            // It doubles as the start of the per-packet cost measurement.
            const uint64_t now = rte_rdtsc();

            for (uint16_t i = 0; i < nb_rx; ++i) {
                frames[i] = rte_pktmbuf_mtod(rx[i], const uint8_t*);
                lens[i] = rte_pktmbuf_data_len(rx[i]);
                rte_prefetch0(frames[i]);
            }
            limiter.classify_burst(frames, lens, nb_rx, now, verdicts);

            uint16_t n_fwd = 0;
            uint16_t n_drop = 0;
            for (uint16_t i = 0; i < nb_rx; ++i) {
                if (verdicts[i] == Verdict::kForward) {
                    fwd[n_fwd++] = rx[i];
                } else {
                    drop[n_drop++] = rx[i];
                }
            }
            send_burst(ctx->inside_port, ctx->queue_id, fwd, n_fwd,
                       ctx->port_counters.tx_failed);
            if (n_drop > 0) {
                rte_pktmbuf_free_bulk(drop, n_drop);
            }

            ctx->perf.packets.add(nb_rx);
            ctx->perf.busy_cycles.add(rte_rdtsc() - now);
        }

        // ── Inside -> outside: return traffic is not limited ──────────────────
        if (ctx->two_ports) {
            const uint16_t nb_ret = rte_eth_rx_burst(ctx->inside_port, ctx->queue_id, rx, kBurst);
            if (nb_ret > 0) {
                ctx->port_counters.passthrough.add(nb_ret);
                send_burst(ctx->outside_port, ctx->queue_id, rx, nb_ret,
                           ctx->port_counters.tx_failed);
            }
        }
    }
    return 0;
}

// Configures `port` with one RX and one TX queue per worker and RSS keyed on
// the IPv4 source so that a given source always lands on the same worker.
int port_init(uint16_t port, uint16_t nb_queues, rte_mempool* pool) {
    rte_eth_dev_info dev_info;
    int ret = rte_eth_dev_info_get(port, &dev_info);
    if (ret != 0) {
        fprintf(stderr, "[fastpath] port %u: dev_info_get failed: %s\n", port, strerror(-ret));
        return ret;
    }
    if (nb_queues > dev_info.max_rx_queues || nb_queues > dev_info.max_tx_queues) {
        fprintf(stderr,
                "[fastpath] port %u (%s) supports %u RX / %u TX queues but %u workers were "
                "requested; start with fewer worker lcores\n",
                port, dev_info.driver_name, dev_info.max_rx_queues, dev_info.max_tx_queues,
                nb_queues);
        return -EINVAL;
    }

    rte_eth_conf conf;
    memset(&conf, 0, sizeof(conf));

    if (nb_queues > 1) {
        // Hash on IPv4 addresses only (no L4 ports) so all of a source's flows
        // share one queue. Where the NIC supports it, restrict further to the
        // source address, which makes the per-source limit exact.
        uint64_t rss_hf = RTE_ETH_RSS_IPV4 & dev_info.flow_type_rss_offloads;
        if (rss_hf == 0) {
            fprintf(stderr,
                    "[fastpath] port %u (%s) cannot RSS on IPv4; run with a single worker\n",
                    port, dev_info.driver_name);
            return -ENOTSUP;
        }
        const bool src_only = (dev_info.flow_type_rss_offloads & RTE_ETH_RSS_L3_SRC_ONLY) != 0;
        if (src_only) {
            rss_hf |= RTE_ETH_RSS_L3_SRC_ONLY;
        } else {
            fprintf(stderr,
                    "[fastpath] WARNING port %u (%s): NIC cannot hash on IPv4 source only; "
                    "hashing on (src, dst). A source talking to several destination IPs may "
                    "be split across cores and get up to %u x --rate in total.\n",
                    port, dev_info.driver_name, nb_queues);
        }
        conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
        conf.rx_adv_conf.rss_conf.rss_key = nullptr;   // driver default key
        conf.rx_adv_conf.rss_conf.rss_hf = rss_hf;
    }

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) {
        // Safe: one mempool, and Fastpath never clones or refcounts mbufs.
        conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
    }

    ret = rte_eth_dev_configure(port, nb_queues, nb_queues, &conf);
    if (ret != 0) {
        fprintf(stderr, "[fastpath] port %u: configure failed: %s\n", port, strerror(-ret));
        return ret;
    }

    uint16_t nb_rxd = kRxDesc;
    uint16_t nb_txd = kTxDesc;
    ret = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (ret != 0) {
        return ret;
    }

    const int socket = rte_eth_dev_socket_id(port);
    rte_eth_txconf txconf = dev_info.default_txconf;
    txconf.offloads = conf.txmode.offloads;
    for (uint16_t q = 0; q < nb_queues; ++q) {
        ret = rte_eth_rx_queue_setup(port, q, nb_rxd, socket, nullptr, pool);
        if (ret < 0) {
            fprintf(stderr, "[fastpath] port %u: rx queue %u setup failed\n", port, q);
            return ret;
        }
        ret = rte_eth_tx_queue_setup(port, q, nb_txd, socket, &txconf);
        if (ret < 0) {
            fprintf(stderr, "[fastpath] port %u: tx queue %u setup failed\n", port, q);
            return ret;
        }
    }

    ret = rte_eth_dev_start(port);
    if (ret < 0) {
        fprintf(stderr, "[fastpath] port %u: start failed: %s\n", port, strerror(-ret));
        return ret;
    }
    // Some virtual PMDs do not implement promiscuous mode; that is fine.
    if (rte_eth_promiscuous_enable(port) != 0) {
        fprintf(stderr, "[fastpath] port %u: promiscuous mode not supported, continuing\n", port);
    }

    rte_ether_addr mac;
    rte_eth_macaddr_get(port, &mac);
    printf("[fastpath] port %u (%s) up: %u queue(s), MAC " RTE_ETHER_ADDR_PRT_FMT "\n", port,
           dev_info.driver_name, nb_queues, RTE_ETHER_ADDR_BYTES(&mac));
    return 0;
}

struct Totals {
    uint64_t allowed = 0, rate_limited = 0, non_ipv4 = 0, malformed = 0, table_full = 0;
    uint64_t new_sources = 0, recycled_sources = 0, tx_failed = 0, passthrough = 0;
    uint64_t tcp = 0, udp = 0, icmp = 0, other_l4 = 0, fragments = 0;
};

Totals collect(const std::vector<WorkerContext*>& workers) {
    Totals t;
    for (const WorkerContext* w : workers) {
        const LimiterCounters& c = w->limiter->counters();
        t.allowed += c.allowed.get();
        t.rate_limited += c.rate_limited.get();
        t.non_ipv4 += c.non_ipv4.get();
        t.malformed += c.malformed.get();
        t.table_full += c.table_full.get();
        t.new_sources += c.new_sources.get();
        t.recycled_sources += c.recycled_sources.get();
        t.tcp += c.tcp.get();
        t.udp += c.udp.get();
        t.icmp += c.icmp.get();
        t.other_l4 += c.other_l4.get();
        t.fragments += c.fragments.get();
        t.tx_failed += w->port_counters.tx_failed.get();
        t.passthrough += w->port_counters.passthrough.get();
    }
    return t;
}

struct WorkerPerf {
    uint64_t packets = 0;
    uint64_t busy_cycles = 0;
};

std::vector<WorkerPerf> collect_perf(const std::vector<WorkerContext*>& workers) {
    std::vector<WorkerPerf> out;
    for (const WorkerContext* w : workers) {
        out.push_back({w->perf.packets.get(), w->perf.busy_cycles.get()});
    }
    return out;
}

// Per-core throughput and cost. To measure the ceiling, offer more traffic
// than the core can take (watch nic_imissed rise): then Mpps is the core's
// maximum and cycles/pkt is what it costs.
void print_perf(const std::vector<WorkerContext*>& workers, const std::vector<WorkerPerf>& now,
                const std::vector<WorkerPerf>& prev, double secs) {
    for (size_t i = 0; i < workers.size(); ++i) {
        const uint64_t pkts = now[i].packets - prev[i].packets;
        const uint64_t cycles = now[i].busy_cycles - prev[i].busy_cycles;
        printf("[fastpath]   lcore %2u q%u: %6.2f Mpps  %6.1f cycles/pkt\n",
               workers[i]->lcore_id, workers[i]->queue_id,
               secs > 0 ? pkts / secs / 1e6 : 0.0,
               pkts > 0 ? static_cast<double>(cycles) / pkts : 0.0);
    }
}

void print_stats(const Totals& now, const Totals& prev, double secs, uint16_t outside_port) {
    auto rate = [secs](uint64_t a, uint64_t b) { return secs > 0 ? (a - b) / secs : 0.0; };
    rte_eth_stats hw;
    memset(&hw, 0, sizeof(hw));
    rte_eth_stats_get(outside_port, &hw);

    printf("[fastpath] allowed %.0f pps | rate-limited %.0f pps | non-ipv4 %.0f pps | "
           "passthrough %.0f pps\n",
           rate(now.allowed, prev.allowed), rate(now.rate_limited, prev.rate_limited),
           rate(now.non_ipv4, prev.non_ipv4), rate(now.passthrough, prev.passthrough));
    printf("[fastpath]   totals: allowed=%" PRIu64 " rate_limited=%" PRIu64
           " malformed=%" PRIu64 " table_full=%" PRIu64 " new_src=%" PRIu64
           " recycled_src=%" PRIu64 " tx_failed=%" PRIu64 " nic_imissed=%" PRIu64 "\n",
           now.allowed, now.rate_limited, now.malformed, now.table_full, now.new_sources,
           now.recycled_sources, now.tx_failed, hw.imissed);
    printf("[fastpath]   ipv4 mix: tcp=%" PRIu64 " udp=%" PRIu64 " icmp=%" PRIu64
           " other=%" PRIu64 " fragments=%" PRIu64 "\n",
           now.tcp, now.udp, now.icmp, now.other_l4, now.fragments);
}

}  // namespace

int main(int argc, char** argv) {
    const int eal_args = rte_eal_init(argc, argv);
    if (eal_args < 0) {
        rte_exit(EXIT_FAILURE, "EAL init failed\n");
    }
    argc -= eal_args;
    argv += eal_args;

    FastpathConfig cfg;
    std::string err;
    if (!parse_fastpath_args(argc, argv, &cfg, &err)) {
        fprintf(stderr, "[fastpath] %s\n%s", err.c_str(), fastpath_usage("aegis-fastpath").c_str());
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }

    BucketParams params;
    if (!BucketParams::make(rte_get_tsc_hz(), cfg.rate_pps, cfg.burst, &params)) {
        rte_exit(EXIT_FAILURE, "--burst %" PRIu64 " is too large for this CPU clock\n", cfg.burst);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    // ── Ports ─────────────────────────────────────────────────────────────────
    uint16_t ports[kMaxPorts];
    uint16_t nb_ports = 0;
    uint16_t port_id;
    RTE_ETH_FOREACH_DEV(port_id) {
        if (nb_ports < kMaxPorts) {
            ports[nb_ports] = port_id;
        }
        ++nb_ports;
    }
    if (nb_ports == 0 || nb_ports > kMaxPorts) {
        rte_exit(EXIT_FAILURE,
                 "found %u ports; Fastpath MVP needs 1 (loopback) or 2 (outside, inside). "
                 "Use -a <pci> or --vdev to select them\n",
                 nb_ports);
    }

    const unsigned nb_workers = rte_lcore_count() - 1;
    if (nb_workers == 0 || nb_workers > UINT16_MAX) {
        rte_exit(EXIT_FAILURE,
                 "need at least 2 lcores (1 main + N workers), e.g. -l 0-2 for 2 workers\n");
    }
    const uint16_t nb_queues = static_cast<uint16_t>(nb_workers);

    // Enough mbufs for every RX/TX ring, every in-flight burst, and the per-lcore caches.
    const unsigned nb_mbufs = std::max(
        8192u, nb_ports * nb_queues * (kRxDesc + kTxDesc + 2 * kBurst) +
                   rte_lcore_count() * kMbufCache);
    rte_mempool* pool = rte_pktmbuf_pool_create("fastpath_mbufs", nb_mbufs, kMbufCache, 0,
                                                RTE_MBUF_DEFAULT_BUF_SIZE,
                                                rte_eth_dev_socket_id(ports[0]));
    if (pool == nullptr) {
        rte_exit(EXIT_FAILURE, "cannot create mbuf pool: %s\n", rte_strerror(rte_errno));
    }

    for (uint16_t i = 0; i < nb_ports; ++i) {
        if (port_init(ports[i], nb_queues, pool) != 0) {
            rte_exit(EXIT_FAILURE, "cannot init port %u\n", ports[i]);
        }
    }

    if (nb_workers != kRecommendedWorkers) {
        printf("[fastpath] note: running %u workers; the reference deployment is %u "
               "(e.g. -l 0-%u, main lcore 0 + workers 1-%u)\n",
               nb_workers, kRecommendedWorkers, kRecommendedWorkers, kRecommendedWorkers);
    }
    printf("[fastpath] %u worker(s), limit %" PRIu64 " pps per source, burst %" PRIu64
           ", %" PRIu64 " flow slots per worker, table-full policy: %s\n",
           nb_workers, cfg.rate_pps, cfg.burst, cfg.flows_per_core,
           cfg.on_table_full == TableFullPolicy::kAllow ? "allow" : "drop");

    // ── Workers: one per lcore, state allocated on that lcore's NUMA node ────
    std::vector<WorkerContext*> workers;
    uint16_t queue = 0;
    unsigned lcore_id;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        const int socket = static_cast<int>(rte_lcore_to_socket_id(lcore_id));
        void* ctx_mem = rte_zmalloc_socket("fastpath_ctx", sizeof(WorkerContext),
                                           RTE_CACHE_LINE_SIZE, socket);
        void* lim_mem = rte_zmalloc_socket("fastpath_limiter", sizeof(SourceLimiter),
                                           RTE_CACHE_LINE_SIZE, socket);
        auto* slots = static_cast<FlowSlot*>(rte_zmalloc_socket(
            "fastpath_flows", cfg.flows_per_core * sizeof(FlowSlot), RTE_CACHE_LINE_SIZE, socket));
        if (ctx_mem == nullptr || lim_mem == nullptr || slots == nullptr) {
            rte_exit(EXIT_FAILURE,
                     "cannot allocate %" PRIu64 " flow slots on socket %d; lower --flows or "
                     "reserve more hugepages\n",
                     cfg.flows_per_core, socket);
        }

        auto* ctx = new (ctx_mem) WorkerContext();
        ctx->lcore_id = lcore_id;
        ctx->queue_id = queue++;
        ctx->outside_port = ports[0];
        ctx->two_ports = nb_ports == 2;
        ctx->inside_port = ctx->two_ports ? ports[1] : ports[0];
        ctx->slots = slots;
        ctx->limiter = new (lim_mem) SourceLimiter(
            slots, static_cast<uint32_t>(cfg.flows_per_core), rte_rand(), params,
            cfg.on_table_full);
        workers.push_back(ctx);
    }

    for (WorkerContext* w : workers) {
        rte_eal_remote_launch(worker_main, w, w->lcore_id);
    }

    // ── Main lcore: stats until signalled ─────────────────────────────────────
    const uint64_t hz = rte_get_tsc_hz();
    Totals prev;
    std::vector<WorkerPerf> prev_perf(workers.size());
    uint64_t prev_tsc = rte_rdtsc();
    while (!g_force_quit.load(std::memory_order_relaxed)) {
        rte_delay_ms(100);
        if (cfg.stats_interval_s == 0) {
            continue;
        }
        const uint64_t now_tsc = rte_rdtsc();
        if (now_tsc - prev_tsc < cfg.stats_interval_s * hz) {
            continue;
        }
        const double secs = static_cast<double>(now_tsc - prev_tsc) / hz;
        const Totals now = collect(workers);
        const std::vector<WorkerPerf> now_perf = collect_perf(workers);
        print_stats(now, prev, secs, ports[0]);
        print_perf(workers, now_perf, prev_perf, secs);
        prev = now;
        prev_perf = now_perf;
        prev_tsc = now_tsc;
    }

    printf("[fastpath] shutting down\n");
    rte_eal_mp_wait_lcore();
    print_stats(collect(workers), Totals{}, 0, ports[0]);

    for (uint16_t i = 0; i < nb_ports; ++i) {
        rte_eth_dev_stop(ports[i]);
        rte_eth_dev_close(ports[i]);
    }
    for (WorkerContext* w : workers) {
        w->limiter->~SourceLimiter();
        rte_free(w->limiter);
        rte_free(w->slots);
        w->~WorkerContext();
        rte_free(w);
    }
    rte_mempool_free(pool);
    rte_eal_cleanup();
    return 0;
}
