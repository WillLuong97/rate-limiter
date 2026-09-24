#!/usr/bin/env python3
"""Writes a pcap of 64-byte UDP frames for the Fastpath pcap demo.

One "attacker" source sends `--attacker` packets and each of `--sources`
well-behaved sources sends `--normal` packets, interleaved. Standard library
only, so it runs anywhere.
"""
import argparse
import struct


def frame(src: int, sport: int) -> bytes:
    eth = b"\x02\x00\x00\x00\x00\x02" + b"\x02\x00\x00\x00\x00\x01" + b"\x08\x00"
    udp = struct.pack("!HHHH", sport, 8080, 8 + 18, 0) + b"\x00" * 18
    ip = struct.pack("!BBHHHBBHII", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0,
                     src, 0x0A640001)  # dst 10.100.0.1
    return eth + ip + udp


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--sources", type=int, default=50)
    ap.add_argument("--normal", type=int, default=20)
    ap.add_argument("--attacker", type=int, default=5000)
    a = ap.parse_args()

    attacker = 0xC6336401  # 198.51.100.1
    pkts = [frame(attacker, 40000 + i % 1000) for i in range(a.attacker)]
    for s in range(a.sources):
        src = 0xCB007100 + s + 1  # 203.0.113.x
        for i in range(a.normal):
            # spread the well-behaved packets through the attack
            pkts.insert((s * a.normal + i) * len(pkts) // (a.sources * a.normal + 1),
                        frame(src, 50000 + i))

    with open(a.out, "wb") as f:
        f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        for i, p in enumerate(pkts):
            f.write(struct.pack("<IIII", 0, i, len(p), len(p)))
            f.write(p)
    print(f"wrote {len(pkts)} packets to {a.out}: attacker 198.51.100.1 x{a.attacker}, "
          f"{a.sources} normal sources x{a.normal}")


if __name__ == "__main__":
    main()
