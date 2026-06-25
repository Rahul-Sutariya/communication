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
"""The simplest possible cross-VM shared-memory example (driven by a C++ binary).

VM-A runs ``simple_ivshmem --write`` to publish a string into the shared ivshmem region,
then VM-B runs ``simple_ivshmem --read`` to read it back. The C++ program lives in
``simple_ivshmem.cpp``; this test only orchestrates the two VMs and prints both results.

Run it and watch BOTH VMs' results with::

    bazel test //quality/integration_testing/environments/dual_qemu/example:test_simple_ivshmem \\
        --config=qnx_x86_64 --test_output=streamed

``--test_output=streamed`` (or ``=all``) is what makes the ``[VM-A ...]`` and
``[VM-B ...]`` lines visible on your console.
"""
import logging

logger = logging.getLogger(__name__)

APP = "bin/simple_ivshmem"
CWD = "/opt/simple_ivshmem"
MESSAGE = "HELLO_FROM_VM_A BMW"


def test_simple_shared_memory_read_write(target_a, target_b):
    """VM-A writes a string with the C++ binary, VM-B reads it back; both results printed.

    The binary reaches the shared ivshmem region through pci-server (--pci), so no
    /dev/ivshmem0 guest driver is required.
    """
    # 1) VM-A WRITES the message into the shared region.
    rc_a, out_a = target_a.execute(f"cd {CWD} && {APP} --write '{MESSAGE}' --pci")
    text_a = out_a.decode(errors="replace").strip()
    logger.info("==================== VM-A (writer) ====================")
    logger.info("%s (rc=%s)", text_a, rc_a)
    print(f"\n[VM-A] {text_a} (rc={rc_a})")

    # 2) VM-B READS the same bytes back from the shared region.
    rc_b, out_b = target_b.execute(f"cd {CWD} && {APP} --read --pci")
    text_b = out_b.decode(errors="replace").strip()
    logger.info("==================== VM-B (reader) ====================")
    logger.info("%s (rc=%s)", text_b, rc_b)
    print(f"[VM-B] {text_b} (rc={rc_b})")

    # 3) Both VMs observed the same data through the shared ivshmem region.
    assert rc_a == 0, f"VM-A write failed (rc={rc_a}): {text_a}"
    assert rc_b == 0, f"VM-B read failed (rc={rc_b}): {text_b}"
    assert MESSAGE in text_b, f"VM-B did not read back {MESSAGE!r}, got: {text_b!r}"
