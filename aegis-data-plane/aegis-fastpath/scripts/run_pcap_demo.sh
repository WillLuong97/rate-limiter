#!/usr/bin/env bash
# End-to-end Fastpath demo with no NIC: DPDK's pcap PMD replays a capture into
# RX and writes whatever Fastpath forwards to TX. Uses --no-huge so it runs in
# an unprivileged container. One worker, since the pcap PMD has one queue.
#
# Expected: every packet from the 50 normal sources is forwarded; the attacker
# gets only --burst packets through (the replay takes far less than a second,
# so its bucket barely refills).
set -euo pipefail

WORK=${WORK:-/tmp/aegis-fastpath-demo}
mkdir -p "$WORK"
make_test_pcap.py "$WORK/in.pcap" --sources 50 --normal 20 --attacker 5000

timeout --signal=INT 5 aegis-fastpath \
    -l 0-1 --no-huge -m 512 --no-pci \
    --vdev "net_pcap0,rx_pcap=$WORK/in.pcap,tx_pcap=$WORK/out.pcap" \
    -- --rate 100 --burst 50 --flows 65536 || true

python3 - "$WORK/in.pcap" "$WORK/out.pcap" <<'PY'
import struct, sys, collections
def count(path):
    c = collections.Counter()
    with open(path, "rb") as f:
        f.read(24)
        while hdr := f.read(16):
            n = struct.unpack("<IIII", hdr)[2]
            pkt = f.read(n)
            c[".".join(map(str, pkt[26:30]))] += 1
    return c
i, o = count(sys.argv[1]), count(sys.argv[2])
atk = "198.51.100.1"
normal_in = sum(v for k, v in i.items() if k != atk)
normal_out = sum(v for k, v in o.items() if k != atk)
print(f"attacker  {atk}: in {i[atk]:6d}  out {o[atk]:6d}")
print(f"50 normal sources   : in {normal_in:6d}  out {normal_out:6d}")
ok = normal_out == normal_in and o[atk] <= 60
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
PY
