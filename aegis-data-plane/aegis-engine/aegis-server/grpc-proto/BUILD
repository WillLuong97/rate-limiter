load("@rules_proto_grpc_cpp//:defs.bzl", "cpp_proto_library", "cpp_grpc_library")

cpp_proto_library(
    name = "udpa_annotations_cc",
    protos = ["@com_github_cncf_xds//udpa/annotations:pkg"],
    visibility = ["//visibility:public"],
)

cpp_proto_library(
    name = "xds_annotations_v3_cc",
    protos = ["@com_github_cncf_xds//xds/annotations/v3:pkg"],
    visibility = ["//visibility:public"],
)

cpp_proto_library(
    name = "xds_core_v3_cc",
    protos = ["@com_github_cncf_xds//xds/core/v3:pkg"],
    deps = [
        ":xds_annotations_v3_cc",
        "@com_envoyproxy_protoc_gen_validate//validate:validate_cc",
    ],
    visibility = ["//visibility:public"],
)

cpp_proto_library(
    name = "envoy_annotations_cc",
    protos = ["@envoy_api//envoy/annotations:pkg"],
    visibility = ["//visibility:public"],
)

cpp_proto_library(
    name = "envoy_type_v3_cc",
    protos = ["@envoy_api//envoy/type/v3:pkg"],
    deps = [
        ":udpa_annotations_cc",
        ":xds_annotations_v3_cc",
        "@com_envoyproxy_protoc_gen_validate//validate:validate_cc",
    ],
    visibility = ["//visibility:public"],
)

cpp_proto_library(
    name = "envoy_type_matcher_v3_cc",
    protos = ["@envoy_api//envoy/type/matcher/v3:pkg"],
    deps = [
        ":envoy_annotations_cc",
        ":envoy_type_v3_cc",
        ":udpa_annotations_cc",
        ":xds_core_v3_cc",
        "@com_envoyproxy_protoc_gen_validate//validate:validate_cc",
    ],
    visibility = ["//visibility:public"],
)

cpp_proto_library(
    name = "envoy_config_core_v3_cc",
    protos = ["@envoy_api//envoy/config/core/v3:pkg"],
    deps = [
        ":envoy_annotations_cc",
        ":envoy_type_matcher_v3_cc",
        ":envoy_type_v3_cc",
        ":udpa_annotations_cc",
        ":xds_annotations_v3_cc",
        ":xds_core_v3_cc",
        "@com_envoyproxy_protoc_gen_validate//validate:validate_cc",
    ],
    visibility = ["//visibility:public"],
)

cpp_proto_library(
    name = "envoy_common_ratelimit_v3_cc",
    protos = ["@envoy_api//envoy/extensions/common/ratelimit/v3:pkg"],
    deps = [
        ":envoy_type_v3_cc",
        ":udpa_annotations_cc",
        "@com_envoyproxy_protoc_gen_validate//validate:validate_cc",
    ],
    visibility = ["//visibility:public"],
)

cpp_grpc_library(
    name = "rls_cc_grpc",
    protos = ["@envoy_api//envoy/service/ratelimit/v3:pkg"],
    deps = [
        ":envoy_config_core_v3_cc",
        ":envoy_common_ratelimit_v3_cc",
        ":udpa_annotations_cc",
        "@com_envoyproxy_protoc_gen_validate//validate:validate_cc",
    ],
    visibility = ["//visibility:public"],
)
