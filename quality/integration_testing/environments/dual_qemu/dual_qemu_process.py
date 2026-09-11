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

import concurrent.futures
import logging
import time

from score.itf.plugins.qemu.qemu_process import QemuProcess
from score.itf.plugins.qemu.qemu_target import QemuTarget

from .ivshmem_qemu import IvshmemQemu

logger = logging.getLogger(__name__)

# "echo ready" answers in ~15 ms on a healthy guest, so anything near this is a wedged one.
_READINESS_EXEC_TIMEOUT_S = 15


def _wait_for_ssh(target, total_timeout: int = 180, interval: int = 3, stable_successes: int = 3):
    """Wait until the VM *stably* serves SSH.

    Early-boot sshd is briefly unstable, so require several consecutive successes before
    calling the VM usable. Reuse one SSH connection for those checks because this guest can
    fail to accept a new connection while an existing one is open.

    ``echo ready`` gets an explicit short timeout rather than score_itf's 30s-start/180s-run
    defaults: a wedged guest accepts the connection and authenticates but then never runs the
    command at all, and on those defaults one such probe burns most of ``total_timeout``,
    leaving the loop barely any retries inside a single boot attempt.
    """
    deadline = time.monotonic() + total_timeout
    last_error = None
    connected = False
    while time.monotonic() < deadline:
        consecutive = 0
        try:
            with target.ssh(timeout=10, n_retries=1, retry_interval=1) as ssh:
                connected = True
                while consecutive < stable_successes:
                    return_code = ssh.execute_command(
                        "echo ready",
                        timeout=_READINESS_EXEC_TIMEOUT_S,
                        max_exec_time=_READINESS_EXEC_TIMEOUT_S,
                    )
                    if return_code != 0:
                        last_error = RuntimeError(f"SSH readiness command failed with exit code {return_code}")
                        break
                    consecutive += 1
                    if consecutive >= stable_successes:
                        return
                    time.sleep(interval)
        except Exception as ex:  # pylint: disable=broad-except
            last_error = ex
        time.sleep(interval)
    if connected:
        raise TimeoutError(
            f"VM accepted SSH but never completed 'echo ready' within {total_timeout}s; sshd "
            f"authenticates but cannot serve a session: {last_error}"
        )
    raise TimeoutError(f"VM never became stably reachable via SSH within {total_timeout}s: {last_error}")


def execute_async_with_retries(target, binary_path, attempts: int = 3, ssh_recovery_timeout_s: int = 30, **kwargs):
    """Launch ``binary_path`` on ``target``, retrying if the SSH session collapses.

    ``QemuTarget.execute_async`` opens a *brand-new* SSH connection per launch, and this guest
    can refuse or drop one shortly after serving another (see ``_wait_for_ssh``); in CI that
    surfaces as ``SSH connection ... failed`` or ``EOFError`` from ``exec_command``. Both abort
    before the remote shell reports its PID, so there is no process handle left to reclaim and
    waiting for sshd to settle before dialling again is the cheapest recovery.
    """
    last_error = None
    for attempt in range(1, attempts + 1):
        if attempt > 1:
            try:
                _wait_for_ssh(target, total_timeout=ssh_recovery_timeout_s, stable_successes=2)
            except Exception as probe_error:  # pylint: disable=broad-except
                logger.warning("VM still not serving SSH before retry %d (%s)", attempt, probe_error)
        try:
            return target.execute_async(binary_path, **kwargs)
        except Exception as ex:  # pylint: disable=broad-except
            last_error = ex
            logger.warning("Launching %s failed on attempt %d/%d (%s)", binary_path, attempt, attempts, ex)
    raise last_error


def stop_quietly(process, label: str = ""):
    """Best-effort ``QemuAsyncProcess.stop()`` that never raises.

    ``stop()`` delivers ``kill`` over yet another fresh SSH connection, so it can fail exactly
    like a launch can. It only runs once the test is already abandoning the VM, so a failure
    here must not replace the real outcome with an SSH error.
    """
    try:
        process.stop()
    except Exception as ex:  # pylint: disable=broad-except
        logger.warning("Could not stop remote process %s cleanly (%s)", label, ex)


def _unresponsive(processes):
    """Return the VMs that are not currently serving SSH.

    The probes only read, so running them concurrently is safe and keeps a slow one from
    adding idle time to its peer -- idle time is exactly what wedges this guest's sshd.
    """
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(processes)) as pool:
        healthy = pool.map(lambda process: process.is_responsive(), processes)
        return [process for process, is_healthy in zip(processes, healthy) if not is_healthy]


def ensure_all_responsive(processes, max_heal_rounds: int = 2):
    """Bring every VM to a responsive state, re-probing after each round of restarts.

    A single probe-then-heal pass is not enough: ``self_heal`` costs a full boot plus the
    stable-SSH wait, and the peer sits idle for all of it, which is what knocks this guest's
    sshd out in the first place. So healing one VM routinely breaks the other. Keep going
    until a round finds nothing left to heal.
    """
    for _ in range(max_heal_rounds):
        unhealthy = _unresponsive(processes)
        if not unhealthy:
            return
        for process in unhealthy:
            process.self_heal()
    still_unhealthy = _unresponsive(processes)
    if still_unhealthy:
        raise RuntimeError(
            f"{len(still_unhealthy)} of {len(processes)} VMs still unresponsive after "
            f"{max_heal_rounds} self-heal rounds"
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
                _wait_for_ssh(self._target, total_timeout=self._boot_timeout)
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

    def is_responsive(self, timeout: int = 60, stable_successes: int = 2) -> bool:
        """Read-only SSH reachability probe; never restarts, so it is safe to run concurrently."""
        try:
            _wait_for_ssh(self._target, total_timeout=timeout, stable_successes=stable_successes)
            return True
        except TimeoutError:
            return False

    def self_heal(self):
        """Restart the VM, reusing ``start()``'s boot-retry and readiness wait."""
        logger.warning("VM went unresponsive; restarting to self-heal")
        self.stop()
        self.start()

    @property
    def target(self):
        """The ``QemuTarget`` for this VM (available after ``start()``)."""
        return self._target
