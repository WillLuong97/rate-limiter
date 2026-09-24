# Aegis Fastpath (MVP)

A DPDK-based L3/L4 fast path that extends Aegis Engine with a second layer of enforcement. It limits each IPv4 source to a packets-per-second budget. The limit is a token bucket, checked at line rate, before traffic reaches the kernel, Envoy, or Redis.

| Layer | Where | Enforces | State |
|---|---|---|---|
| **Aegis Fastpath** (this) | On the wire, in front of the gateway | Packets/sec per source IP (L3/L4) | Per-core memory, lock-free |
| **Aegis Engine** | Envoy RLS → gRPC → Redis Cluster | Requests/sec per client IP (L7) | Shared in Redis, cluster-wide |

Fastpath sheds floods cheaply so that Engine only spends its Redis round trips on traffic that is plausibly legitimate. Floods include UDP/ICMP spray, SYN floods, and a single host hammering the gateway.

## Architecture

```
                 NIC (port 0, "outside")
                 RSS: hash on IPv4 source
      ┌──────────────┬──────────────┬──────────────┐
   RX queue 0     RX queue 1     RX queue 2     RX queue 3
      │              │              │              │
  worker lcore 1 worker lcore 2 worker lcore 3 worker lcore 4    (pinned by EAL, 1 CPU each)
  ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐
  │parse L2-4│   │   ...    │   │   ...    │   │   ...    │
  │FlowTable │   │FlowTable │   │FlowTable │   │FlowTable │    (own memory, own NUMA node)
  │ buckets  │   │          │   │          │   │          │
  └────┬─────┘   └────┬─────┘   └────┬─────┘   └────┬─────┘
   TX queue 0     TX queue 1     TX queue 2     TX queue 3
                 port 1 ("inside") → Envoy / Aegis Engine

  main lcore 0: stats (per-core Mpps, cycles/pkt), SIGINT/SIGTERM
```

- **RSS flow steering.** The NIC hashes on IPv4 addresses only, not L4 ports. That sends every packet from a source to the same RX queue, and so to the same core. When the NIC advertises `RTE_ETH_RSS_L3_SRC_ONLY`, Fastpath hashes on the source alone and the per-source limit is exact.
- **Lock-free, shared-nothing per-core state.** Each worker owns its `FlowTable`, and no other core ever writes to it. The hot path has no locks, no atomic read-modify-write, and no shared cache lines. Stats counters are single-writer relaxed atomics, which compile to plain loads and stores.
- **Run-to-completion.** Each worker runs RX burst (32) → parse → pass 1 (hash and prefetch flow slots for the whole burst) → pass 2 (bucket check) → TX burst → bulk-free the drops. The two passes overlap the DRAM misses of a big flow table instead of paying them one at a time.
- **Token bucket.** The bucket is integer fixed-point and driven by the TSC. It has no floating point and no syscalls, and it reads `rte_rdtsc()` once per burst. New sources start with a full `--burst`.
- **Flow table.** Open addressing over hugepage memory: 32-byte slots, two per cache line, a probe cap of 16, and a seeded hash that resists collision attacks. Slots are never deleted. A bucket that has been idle long enough to refill completely looks the same as a new one, so its slot can be recycled safely.
- **L3/L4 validation.** The parser checks IPv4 version, IHL and total length. It also checks TCP data offset and the minimum UDP/ICMP header lengths, looking through up to 2 VLAN tags. Malformed IPv4 is dropped, so it can't be used to bypass the limiter. Non-first fragments are charged to their source. Non-IPv4 traffic (ARP, IPv6, …) passes through untouched.

### Ports

- **2 ports (bump in the wire):** port 0 faces clients and is rate-limited. Traffic arriving on port 1 is return traffic and is forwarded to port 0 unlimited.
- **1 port (lab):** limited traffic goes back out the same port.

## Layout

```
src/packet_token_bucket.h   fixed-point token bucket (no DPDK)
src/flow_table.h            per-core src-IP → bucket table (no DPDK)
src/packet_parse.h          Ethernet/VLAN/IPv4/TCP/UDP/ICMP parsing (no DPDK)
src/source_limiter.h        burst pipeline: parse → prefetch → decide (no DPDK)
src/fastpath_config.*       CLI options (no DPDK)
src/fastpath_main.cc        DPDK: EAL, ports + RSS, pinned workers, stats
tests/                      gtest unit tests for everything above except main
bench/limiter_bench.cc      single-core benchmark of the decision path
scripts/                    pcap generator + end-to-end pcap demo
```

Everything except `fastpath_main.cc` builds and tests on macOS without DPDK.

## Build and test

```bash
# Anywhere (macOS/Linux): core library, tests, benchmark
cmake -S . -B build && cmake --build build -j
./build/fastpath_tests
./build/limiter_bench 4000000 8388608 50000000 3.0   # sources flows packets GHz

# Linux with DPDK (apt install dpdk-dev libpcap-dev): also builds ./build/aegis-fastpath

# Or everything in Docker (runs the tests during the build)
docker build -t aegis-fastpath .
docker run --rm aegis-fastpath run_pcap_demo.sh     # end-to-end, no NIC, no hugepages
```

## Run on real NICs (reference deployment: 4 workers)

```bash
# Host prep (once): hugepages, isolated cores, NIC bound to vfio-pci (mlx5 uses its own bifurcated driver instead)
echo 2048 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
# kernel cmdline: isolcpus=2-5 nohz_full=2-5 rcu_nocbs=2-5
sudo dpdk-devbind.py -b vfio-pci 0000:3b:00.0 0000:3b:00.1

# lcore 1 = main/stats; lcores 2-5 = the 4 pinned workers → 4 RSS queues per port
sudo ./build/aegis-fastpath -l 1-5 -n 4 -a 0000:3b:00.0 -a 0000:3b:00.1 -- \
     --rate 1000 --burst 2000 --flows 1048576 --on-table-full allow
```

| Option | Default | Meaning |
|---|---|---|
| `--rate` | 1000 | sustained packets/sec per source IPv4 |
| `--burst` | 2000 | bucket depth, in packets |
| `--flows` | 1048576 | flow table slots per worker (power of 2; 32 B each) |
| `--on-table-full` | `allow` | fail open (same as Engine when Redis is down) or `drop` |
| `--stats-interval` | 1 | seconds between stats lines; 0 disables |

## Measuring throughput and cycles/packet

The target is **~12 Mpps per core at ~250 cycles/packet**. At 3.0 GHz those two numbers describe the same thing. The app reports both for each worker every stats interval:

```
[fastpath]   lcore  2 q0:  12.10 Mpps   247.9 cycles/pkt
```

`cycles/pkt` counts only bursts that carried packets, from `rte_rdtsc()` before classification to after TX and free. It is the real per-packet cost, not diluted by idle polling. To find a core's ceiling:

1. Drive 64-byte packets from many sources with TRex or pktgen-dpdk, faster than the cores can absorb. For example, 4 × 12 Mpps ≈ 48 Mpps. That needs a 40/100 GbE NIC, because 10 GbE tops out at 14.88 Mpps in total.
2. Confirm `nic_imissed` is rising. That means the cores are the bottleneck, so the Mpps shown is their maximum.
3. Read Mpps and cycles/pkt for each lcore. Also run with `--flows` large and many sources, because a cache-resident table flatters the numbers.

`limiter_bench` isolates the parse/table/bucket part without RX/TX. As a rough guide, on an Apple M-series laptop it measured about 30 cycles/pkt with 1k sources and about 70 cycles/pkt with 4M sources in an 8M-slot table. The rest of the 250-cycle budget is RX/TX descriptor handling and mbuf management in the PMD. **None of these numbers have been measured on a DPDK NIC yet.** Measure before quoting them.

## MVP limitations / next steps

- **IPv4 only.** IPv6 passes through unlimited. Add a 128-bit (or /64) keyed table.
- **RSS precision depends on the NIC.** Without `L3_SRC_ONLY`, hashing uses (src, dst). A source that reaches several destination IPs can then land on several cores, and its total can reach N × `--rate` (the app warns at startup). Where `L3_SRC_ONLY` is available, check that the PMD applies it to TCP/UDP packets too. On pctype-based NICs such as i40e/ice this may need an explicit `rte_flow` RSS rule.
- **Static config.** There is one global rate, with no per-prefix or allow-list rules, and no hot reload. The natural next step is for the Aegis control plane to push rules, which workers pick up through an RCU (read-copy-update) or epoch swap.
- **No telemetry export.** Stats go to stdout. Export them to Prometheus alongside Engine's planned metrics.
- **Single mempool** on port 0's NUMA node. On a dual-socket host, use one mempool per socket.
- **No bridge back to Engine.** A source that Engine rejects at L7 could be pushed down to Fastpath as a temporary per-source drop.
