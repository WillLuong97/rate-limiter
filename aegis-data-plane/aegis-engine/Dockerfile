# syntax=docker/dockerfile:1

##### Stage 1 — build redis-plus-plus from source, then compile Aegis Engine with Bazel #####
FROM ubuntu:22.04 AS builder
ARG TARGETARCH
ENV DEBIAN_FRONTEND=noninteractive


RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git curl ca-certificates pkg-config python3 \
    && rm -rf /var/lib/apt/lists/*


# Bazelisk resolves whatever Bazel version this module needs
RUN curl -fsSL -o /usr/local/bin/bazel \
      https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-${TARGETARCH} \
    && chmod +x /usr/local/bin/bazel

# Build hiredis from source at v1.3.0 — the SAME version Bazel's @hiredis module
# uses — so redis-plus-plus's cmake build (below) and the Bazel-compiled code
# agree on struct layout. apt's libhiredis-dev (0.14.1) is a different, incompatible ABI.
RUN git clone --depth 1 --branch v1.3.0 https://github.com/redis/hiredis.git /tmp/hiredis \
    && cd /tmp/hiredis && make -j"$(nproc)" && make install && ldconfig \
    && rm -rf /tmp/hiredis

#redis-plus-plus isn't in the BCR (see MODULE.bazel local_path_override) —
# build + install it so third_party/redis_plus_plus/{include,lib} symlinks resolve
RUN git clone --depth 1 https://github.com/sewenew/redis-plus-plus.git /tmp/redis-plus-plus \
    && mkdir /tmp/redis-plus-plus/build && cd /tmp/redis-plus-plus/build \
    && cmake .. -DREDIS_PLUS_PLUS_BUILD_TEST=OFF \
    && make -j"$(nproc)" && make install && ldconfig \
    && rm -rf /tmp/redis-plus-plus

WORKDIR /app
COPY . .


# Pulls boost/grpc/protobuf/envoy_api/xds from the Bazel Central Registry + GitHub —
# needs network egress during `docker build` and is slow on first build.
RUN --mount=type=cache,target=/root/.cache/bazel \
    bazel build //:aegis-engine-main -c opt \
    && cp bazel-bin/aegis-engine-main /tmp/aegis-engine-main

##### Stage 2 — minimal runtime image #####
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libhiredis0.14 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/lib/libredis++.* /usr/local/lib/
RUN ldconfig
COPY --from=builder /tmp/aegis-engine-main /usr/local/bin/aegis-engine

EXPOSE 8081
ENTRYPOINT ["/usr/local/bin/aegis-engine"]