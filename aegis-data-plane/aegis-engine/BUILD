load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

# ─Core Aegis rate limiting library───────────────────────────────────────────────────────
cc_library(
    name = "rate_limiting_lib",
    srcs = ["rate-limiting-algorithm/token_bucket.cc"],
    hdrs = ["rate-limiting-algorithm/rate_limiter.h",
            "rate-limiting-algorithm/token_bucket.h"],
    deps = [
        "@redis_plus_plus//:redis++", # redis-plus-plus RedisCluster 
        "@hiredis//:hiredis", # underlying hiredis (redis-plus-plus needs it)
        ],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

# ─gRPC server library ───────────────────────────────────────────────────────
cc_library(
    name = "aegis_grpc_server_lib",
    srcs = ["aegis-server/aegis_grpc_server.cpp"],
    hdrs = ["aegis-server/aegis_grpc_server.h"],
    deps = [
        ":rate_limiting_lib", 
        "//aegis-server/grpc-proto:rls_cc_grpc",            
        "@grpc//:grpc++",                       # grpc::Status, grpc::ServerContext
        "@grpc//:grpc++_reflection",            # optional: enables grpc_cli inspection
        ],
    copts = ["-std=c++17"],
)

cc_binary(
    name = "aegis-engine-main",
    srcs = ["aegis_engine_main.cc"],
    deps = [
        "@boost.program_options",
        ":rate_limiting_lib", 
        ":aegis_grpc_server_lib"
    ],
    copts = ["-std=c++17"],
    linkopts = ["-lpthread"],
)

# ── Unit tests ────────────────────────────────────────────────────────────────
cc_test(
    name = "token_bucket_test",
    srcs = ["test/token_bucket_test.cc"],
    deps = [
        ":rate_limiting_lib",
        "@googletest//:gtest_main",
    ],
    copts = ["-std=c++17"],
    linkopts = ["-lpthread"],
    size = "small",
)

