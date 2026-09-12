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
"""Process wrapper that extends :class:`QemuProcess` with ivshmem and boot-retry logic.

Subclasses ``QemuProcess`` and replaces its internal ``_qemu`` with an
:class:`IvshmemQemu` instance so the VM is launched with an ``ivshmem-plain`` device.

The ``start()`` method is self-healing: it waits for stable SSH and restarts the QEMU
process up to ``max_boot_attempts`` times if sshd never comes up.
"""

import logging
import socket
import time

from score.itf.plugins.qemu.qemu_process import QemuProcess
from score.itf.plugins.qemu.qemu_target import QemuTarget

from .ivshmem_qemu import IvshmemQemu

logger = logging.getLogger(__name__)


def _wait_for_sshd_banner(
    host_port: int,
    total_timeout: int = 180,
    poll_interval: float = 0.5,
):
    """Wait until sshd inside the VM is accepting TCP connections and serves the SSH banner.

    Connects via raw TCP socket and checks for the SSH protocol identification string
    (e.g., b"SSH-2.0-..."). This avoids opening a full SSH/Paramiko session during boot,
    preventing SSH session churn and 'Connection reset by peer' / 'No existing session' errors.
    """
    deadline = time.monotonic() + total_timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", host_port), timeout=1.0) as sock:
                sock.settimeout(5.0)
                sock.sendall(b"SSH-2.0-score-itf-readiness\r\n")
                banner = bytearray()
                while len(banner) < 255:
                    chunk = sock.recv(1)
                    if not chunk:
                        break
                    banner.extend(chunk)
                    if chunk == b"\n":
                        break
                banner_line = bytes(banner).rstrip(b"\r\n")
                if banner_line.startswith(b"SSH-"):
                    return
                last_error = RuntimeError(
                    f"Unexpected banner received on port {host_port}: {banner_line!r}"
                )
        except Exception as ex:  # pylint: disable=broad-except
            last_error = ex
        time.sleep(poll_interval)
    raise TimeoutError(
        f"VM sshd never served SSH protocol banner on port {host_port} within {total_timeout}s: {last_error}"
    )


class DualQemuProcess(QemuProcess):
    """A :class:`QemuProcess` subclass with ivshmem support and self-healing boot.

    Use as a context manager::

        with DualQemuProcess(...) as process:
            target = process.target
            ...
    """

    def __init__(
        self,
        path_to_qemu_image,
        available_ram,
        available_cores,
        vm_config,
        port_forwarding=[],
        ivshmem_path=None,
        ivshmem_size="4M",
        intervm=None,
        vm_index=0,
        max_boot_attempts=3,
        boot_timeout=180,
    ):
        super().__init__(
            path_to_qemu_image,
            available_ram,
            available_cores,
            network_adapters=[],
            port_forwarding=port_forwarding,
            machine=vm_config.qemu_machine,
            rootfs=None,
            kernel_cmdline=vm_config.qemu_kernel_cmdline,
        )
        # Replace the base's default Qemu with our ivshmem-capable subclass.
        self._qemu = IvshmemQemu(
            path_to_qemu_image,
            available_ram,
            available_cores,
            network_adapters=[],
            port_forwarding=port_forwarding,
            ivshmem_path=ivshmem_path,
            ivshmem_size=ivshmem_size,
            intervm=intervm,
            vm_index=vm_index,
        )
        self._vm_config = vm_config
        self._max_boot_attempts = max_boot_attempts
        self._boot_timeout = boot_timeout
        self._target = None

    def start(self):
        """Boot the VM, retrying up to ``max_boot_attempts`` times if sshd never serves."""
        last_error = None
        for attempt in range(1, self._max_boot_attempts + 1):
            super().start()
            try:
                self._target = QemuTarget(self, self._vm_config)
                _wait_for_sshd_banner(self._vm_config.ssh_port, total_timeout=self._boot_timeout)
                return self
            except Exception as ex:  # pylint: disable=broad-except
                last_error = ex
                logger.warning(
                    "VM boot attempt %d/%d did not reach a usable state (%s); restarting",
                    attempt,
                    self._max_boot_attempts,
                    ex,
                )
                try:
                    self.stop()
                except Exception:  # pylint: disable=broad-except
                    logger.exception("Failed to stop the wedged QEMU before retrying")
                if attempt < self._max_boot_attempts:
                    logger.info("Waiting 5 s before next boot attempt to let resources settle")
                    time.sleep(5)
        raise RuntimeError(
            f"VM never booted into a usable state after {self._max_boot_attempts} attempts: {last_error}"
        )

    def is_responsive(self, timeout: int = 60) -> bool:
        """Read-only TCP reachability probe (no restart); safe to run concurrently for both VMs."""
        try:
            _wait_for_sshd_banner(self._vm_config.ssh_port, total_timeout=timeout)
            return True
        except TimeoutError:
            return False

    def self_heal(self):
        """Restart the VM, reusing start()'s own boot-retry + readiness-check loop.

        A VM that already passed ``start()`` can still stop serving sshd while it sits idle during
        the peer's boot; one restart recovers it without needing a full outer Bazel retry, which
        would reboot both VMs from scratch.
        """
        logger.warning("VM went unresponsive; restarting to self-heal")
        self.stop()
        self.start()

    def ensure_responsive(self, timeout: int = 60, stable_successes: int = 2):
        """Re-verify the VM is still reachable, restarting it (self-heal) if it went idle-dead."""
        if not self.is_responsive(timeout):
            self.self_heal()

    @property
    def target(self):
        """The ``QemuTarget`` for this VM (available after ``start()``)."""
        return self._target
