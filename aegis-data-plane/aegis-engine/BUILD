# cc_binary(
#     name = "redis-cache", 
#     srcs = ["redis/redis_main.cc"],
#     deps = [
#         "@boost"
#     ]
# )
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "aegis_grpc_server_lib",
    srcs = ["aegis-server/aegis_grpc_server.cpp"],
    hdrs = ["aegis-server/aegis_grpc_server.h"],
    deps = [":token_bucket_lib"],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "token_bucket_lib",
    srcs = ["rate-limiting-algorithm/token_bucket.cc"],
    hdrs = ["rate-limiting-algorithm/Bucket.h",
            "rate-limiting-algorithm/rate_limiter.h",
            "rate-limiting-algorithm/token_bucket.h"],
    deps = ["@redis_plus_plus//:redis++"],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "aegis-engine-main",
    srcs = ["aegis_engine_main.cc"],
    deps = [
        "@boost.program_options",
        ":token_bucket_lib", 
        ":aegis_grpc_server_lib"
    ],
    copts = ["-std=c++17"],
    linkopts = ["-lpthread"],
)

cc_test(
    name = "token_bucket_test",
    srcs = ["test/token_bucket_test.cc"],
    deps = [
        ":token_bucket_lib",
        "@googletest//:gtest_main",
    ],
)

