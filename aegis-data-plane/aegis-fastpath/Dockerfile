# syntax=docker/dockerfile:1
# Builds Aegis Fastpath against the distro DPDK (Ubuntu 24.04 ships DPDK 23.11 LTS).
#
#   docker build -t aegis-fastpath .
#   docker run --rm aegis-fastpath fastpath_tests          # unit tests
#   docker run --rm aegis-fastpath run_pcap_demo.sh         # end-to-end, no NIC needed
#
# Running on real NICs needs hugepages and device access on the host, e.g.
#   docker run --rm --privileged -v /dev/hugepages:/dev/hugepages \
#       aegis-fastpath aegis-fastpath -l 0-4 -a 0000:3b:00.0 -- --rate 1000

FROM ubuntu:24.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config ca-certificates dpdk-dev libpcap-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# The image may run on a different CPU than the builder, so no -march=native.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAEGIS_FASTPATH_NATIVE=OFF \
    && cmake --build build -j"$(nproc)" \
    && ./build/fastpath_tests

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        dpdk libpcap0.8t64 python3 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=builder /src/build/aegis-fastpath /src/build/fastpath_tests \
     /src/build/limiter_bench /usr/local/bin/
COPY scripts/ /usr/local/bin/
ENTRYPOINT []
CMD ["aegis-fastpath", "--help"]
