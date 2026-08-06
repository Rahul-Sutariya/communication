# *******************************************************************************
# Copyright (c) 2026 Contributors to the Eclipse Foundation
#
# See the NOTICE file(s) distributed with this work for additional
# information regarding copyright ownership.
#
# This program and the accompanying materials are made available under the
# terms of the Apache License Version 2.0 which is available at
# https://www.apache.org/licenses/LICENSE-2.0
#
# SPDX-License-Identifier: Apache-2.0
# *******************************************************************************
load("@rules_cc//cc:defs.bzl", "cc_library")
load("@score_baselibs//score/language/safecpp:toolchain_features.bzl", "COMPILER_WARNING_FEATURES")
load("//quality/unit_testing:unit_testing.bzl", "cc_unit_test")

cc_library(
    name = "ivshmem_support",
    srcs = [
        "ivshmem_bar_discovery.cpp",
        "ivshmem_typed_memory_provider.cpp",
    ],
    hdrs = [
        "ivshmem_bar_discovery.h",
        "ivshmem_typed_memory_provider.h",
    ],
    features = COMPILER_WARNING_FEATURES + [
        "aborts_upon_exception",
    ],
    linkopts = select({
        "@platforms//os:qnx": ["-lpci"],
        "//conditions:default": [],
    }),
    visibility = ["//score/mw/com/gateway:__subpackages__"],
    deps = [
        "@score_baselibs//score/memory/shared",
        "@score_baselibs//score/memory/shared/typedshm/typedshm_wrapper:typedmemory",
        "@score_baselibs//score/os:errno",
    ],
)

cc_library(
    name = "ivshmem_typed_memory_provider_mock",
    hdrs = ["ivshmem_typed_memory_provider_mock.h"],
    features = COMPILER_WARNING_FEATURES,
    visibility = ["//score/mw/com/gateway:__subpackages__"],
    deps = [
        ":ivshmem_support",
        "@googletest//:gtest",
    ],
)

cc_unit_test(
    name = "ivshmem_typed_memory_provider_test",
    srcs = ["ivshmem_typed_memory_provider_test.cpp"],
    features = COMPILER_WARNING_FEATURES,
    deps = [
        ":ivshmem_support",
        "@score_baselibs//score/mw/log",
    ],
)

cc_unit_test(
    name = "ivshmem_bar_discovery_test",
    srcs = ["ivshmem_bar_discovery_test.cpp"],
    features = COMPILER_WARNING_FEATURES,
    deps = [
        ":ivshmem_support",
        "@score_baselibs//score/mw/log",
    ],
)
