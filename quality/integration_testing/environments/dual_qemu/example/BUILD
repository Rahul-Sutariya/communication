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
load("@rules_cc//cc:defs.bzl", "cc_binary")
load("@score_baselibs//score/language/safecpp:toolchain_features.bzl", "COMPILER_WARNING_FEATURES")
load("//quality/integration_testing:integration_testing.bzl", "dual_qemu_integration_test")
load("//score/mw/com/test:pkg_application.bzl", "pkg_application")

# Minimal C++ example: one binary that either writes a string into the shared ivshmem
# region (--write) or reads it back (--read). No mw::com involved.
cc_binary(
    name = "simple_ivshmem",
    srcs = ["simple_ivshmem.cpp"],
    features = COMPILER_WARNING_FEATURES + [
        "aborts_upon_exception",
    ],
    # On QNX the program reaches the ivshmem shared memory through pci-server, which
    # requires the PCI access library.
    linkopts = select({
        "@platforms//os:qnx": ["-lpci"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)

pkg_application(
    name = "simple_ivshmem-pkg",
    app_name = "simple_ivshmem",
    bin = [":simple_ivshmem"],
    visibility = ["//visibility:public"],
)

# Boots two QNX VMs sharing one ivshmem region, runs the C++ writer on VM-A and the C++
# reader on VM-B, and prints both results.
dual_qemu_integration_test(
    name = "test_simple_ivshmem",
    srcs = ["simple_ivshmem_test.py"],
    filesystem = ":simple_ivshmem-pkg",
)
