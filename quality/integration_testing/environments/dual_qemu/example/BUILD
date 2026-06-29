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
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")
load("@score_baselibs//score/language/safecpp:toolchain_features.bzl", "COMPILER_WARNING_FEATURES")
load("//quality/integration_testing:integration_testing.bzl", "dual_qemu_integration_test")
load("//score/mw/com/test:pkg_application.bzl", "pkg_application")

# Reusable building block: a custom score::memory::shared::TypedMemory provider that backs
# named shared-memory objects with the ivshmem PCI BAR (via QNX shm_ctl(SHMCTL_PHYS)), plus a
# pci-server helper that discovers the BAR. Injected through
# SharedMemoryFactory::SetTypedMemoryProvider(), it lets the production lib/memory stack run
# directly over the cross-VM ivshmem region. Intended for reuse by the gateway integration
# test.
cc_library(
    name = "ivshmem_typed_memory",
    srcs = ["ivshmem_typed_memory.cpp"],
    hdrs = ["ivshmem_typed_memory.h"],
    features = COMPILER_WARNING_FEATURES,
    # The pci-server discovery helper needs the QNX PCI access library.
    linkopts = select({
        "@platforms//os:qnx": ["-lpci"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
    deps = [
        # typed_memory.h (re-)exports user_permission.h and score/expected.hpp transitively.
        "@score_baselibs//score/memory/shared/typedshm/typedshm_wrapper:typedmemory",
        "@score_baselibs//score/os:errno",
    ],
)

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

# Stage 1 full-stack smoke test: runs the PRODUCTION lib/memory stack (SharedMemoryFactory +
# OffsetPtr) over the ivshmem BAR via the BarTypedMemory provider, on a single VM. Proves the
# shm_ctl(SHMCTL_PHYS) binding + the prefer_typed_memory path actually work end to end.
cc_binary(
    name = "lola_ivshmem",
    srcs = ["lola_ivshmem.cpp"],
    features = COMPILER_WARNING_FEATURES + [
        "aborts_upon_exception",
    ],
    # pci-server discovery (in :ivshmem_typed_memory) needs the PCI access library.
    linkopts = select({
        "@platforms//os:qnx": ["-lpci"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
    deps = [
        ":ivshmem_typed_memory",
        # ":shared" is the public umbrella that exports shared_memory_factory + resource.
        "@score_baselibs//score/memory/shared:offset_ptr",
        "@score_baselibs//score/memory/shared:shared",
        # lib/memory logs via mw::log; link a concrete recorder backend (CreateRecorderFactory).
        "@score_baselibs//score/mw/log:console",
    ],
)

pkg_application(
    name = "lola_ivshmem-pkg",
    app_name = "lola_ivshmem",
    bin = [":lola_ivshmem"],
    visibility = ["//visibility:public"],
)

# Boots the dual-VM environment but only exercises the lib/memory stack on VM-A (single-VM
# Stage 1). The cross-VM exchange is added as a separate Stage 2 test once this passes.
dual_qemu_integration_test(
    name = "test_lola_ivshmem",
    srcs = ["lola_ivshmem_test.py"],
    filesystem = ":lola_ivshmem-pkg",
)
