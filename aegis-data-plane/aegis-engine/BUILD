# cc_binary(
#     name = "redis-cache", 
#     srcs = ["redis/redis_main.cc"],
#     deps = [
#         "@boost"
#     ]
# )
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "token_bucket_lib",
    srcs = ["rate-limiting-algorithm/token_bucket.cc"],
    hdrs = ["rate-limiting-algorithm/token_bucket.h"],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "aegis-engine-main",
    srcs = ["aegis_engine_main.cc"],
    deps = [
        "@boost.program_options",
        ":token_bucket_lib"
    ]
)