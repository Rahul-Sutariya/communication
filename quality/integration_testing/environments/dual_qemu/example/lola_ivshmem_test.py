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
"""Stage 1: run the production lib/memory stack over the ivshmem BAR on a single VM.

The C++ binary (``lola_ivshmem.cpp``) injects the BarTypedMemory provider into
``SharedMemoryFactory``, creates a shared-memory region with ``prefer_typed_memory=true`` so
the object is bound to the ivshmem BAR via ``shm_ctl(SHMCTL_PHYS)``, builds a singly-linked
list of ``score::memory::shared::OffsetPtr`` nodes through the resource, and walks it back.

This is the de-risking step for the gateway integration test: it proves the
``shm_ctl(SHMCTL_PHYS)`` binding and the ``prefer_typed_memory`` path work end to end. The
binary fails hard (non-zero exit) if the object did NOT end up in typed memory, so a green
test really means the data landed on the BAR.

Run it with::

    bazel test //quality/integration_testing/environments/dual_qemu/example:test_lola_ivshmem \\
        --config=qnx_x86_64 --test_output=streamed
"""
import logging

logger = logging.getLogger(__name__)

APP = "bin/lola_ivshmem"
CWD = "/opt/lola_ivshmem"


def test_lola_ivshmem_lib_memory_over_bar(target_a):
    """Create shared memory on the BAR via lib/memory, build + walk an OffsetPtr list."""
    # Redirect stderr->stdout so abort()/assert diagnostics are captured in `out`.
    rc, out = target_a.execute(f"cd {CWD} && {APP} 2>&1")
    text = out.decode(errors="replace").strip()
    logger.info("==================== VM-A ====================")
    logger.info("%s (rc=%s)", text, rc)
    print(f"\n[VM-A] {text} (rc={rc})")

    assert rc == 0, f"lola_ivshmem failed (rc={rc}): {text}"
    # The binary only returns 0 after asserting IsShmInTypedMemory(); these strings confirm
    # the typed-memory (BAR) path and the OffsetPtr round-trip.
    assert "Create OK in typed memory" in text, f"object not in typed memory, got: {text!r}"
    assert "via lib/memory" in text, f"OffsetPtr list not verified, got: {text!r}"
