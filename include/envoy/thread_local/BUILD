licenses(["notice"])  # Apache 2

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_package",
)

envoy_package()

envoy_cc_library(
    name = "thread_local_interface",
    hdrs = ["thread_local.h"],
    deps = ["//include/envoy/event:dispatcher_interface"],
)
