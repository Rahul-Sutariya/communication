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

The ``start()`` method is self-healing: it gates on a serial-console boot marker, verifies
SSH once, and restarts the QEMU process up to ``max_boot_attempts`` times if either fails.
"""

import concurrent.futures
import logging
import socket
import threading
import time

from score.itf.plugins.qemu.qemu_process import QemuProcess
from score.itf.plugins.qemu.qemu_target import QemuTarget

from .ivshmem_qemu import IvshmemQemu

logger = logging.getLogger(__name__)

# "echo ready" answers in ~15 ms on a healthy guest, so anything near this is a wedged one.
_READINESS_EXEC_TIMEOUT_S = 15
# QemuTarget.execute_async connects, opens a channel and reads a PID; seconds on a healthy guest.
_LAUNCH_TIMEOUT_S = 90
# How long an abandoned SSH worker gets to unwind after its client has been closed.
_CANCEL_GRACE_S = 5
# Printed by startup.sh once sshd has bound port 22; see qnx8_qemu/init_x86_64.build.
BOOT_COMPLETE_MARKER = "S-CORE BOOT COMPLETE"


def _call_with_watchdog(func, timeout: float, on_timeout=None):
    """Run ``func`` on a worker thread and give up on it after ``timeout`` seconds.

    paramiko waits on an *un-timed* ``Event`` for the server's reply to a channel-open or
    exec request, so neither score_itf's ``timeout``/``max_exec_time`` nor paramiko's own
    ``settimeout`` bound it. A guest that accepts TCP and authenticates but can no longer
    spawn a session therefore blocks the caller forever -- in CI that consumed the whole
    bazel test timeout on a single ``echo ready``. ``on_timeout`` closes the SSH client,
    which sets the event the worker is parked on so it can unwind.
    """
    outcome = {}

    def _run():
        try:
            outcome["value"] = func()
        except BaseException as ex:  # pylint: disable=broad-except
            outcome["error"] = ex

    worker = threading.Thread(target=_run, name="dual-qemu-ssh", daemon=True)
    worker.start()
    worker.join(timeout)
    if worker.is_alive():
        if on_timeout is not None:
            try:
                on_timeout()
            except Exception:  # pylint: disable=broad-except
                logger.debug("Could not cancel the stuck SSH operation", exc_info=True)
        worker.join(_CANCEL_GRACE_S)
        raise TimeoutError(f"SSH operation did not complete within {timeout:.0f}s")
    if "error" in outcome:
        raise outcome["error"]
    return outcome["value"]


def _readiness_session(ssh_ctx, stable_successes: int, interval: int):
    """Open one SSH session and run ``echo ready`` ``stable_successes`` times over it.

    Reuse a single connection for all checks: each fresh connection is an extra pre-auth
    slot on the guest's sshd, and early-boot sshd is exactly when those are scarcest.
    """
    try:
        ssh = ssh_ctx.__enter__()
    except Exception:
        # score_itf's Ssh.__enter__ retries connect() on one SSHClient and never closes it
        # after giving up, leaving a connected but unauthenticated socket on the guest.
        # Those accumulate until sshd's MaxStartups starts resetting *new* connections,
        # which is what makes a merely slow VM look permanently dead.
        ssh_ctx.__exit__(None, None, None)
        raise
    try:
        for check in range(stable_successes):
            if check:
                time.sleep(interval)
            return_code = ssh.execute_command(
                "echo ready",
                timeout=_READINESS_EXEC_TIMEOUT_S,
                max_exec_time=_READINESS_EXEC_TIMEOUT_S,
            )
            if return_code != 0:
                raise RuntimeError(f"SSH readiness command failed with exit code {return_code}")
    finally:
        ssh_ctx.__exit__(None, None, None)


def _wait_for_sshd_banner(
    host_port: int,
    total_timeout: int = 280,
    poll_interval: float = 10,
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
                sock.settimeout(9.0)
                time.sleep(poll_interval)
                banner = sock.recv(64)
                if banner.startswith(b"SSH-"):
                    return
                last_error = RuntimeError(f"Unexpected banner received on port {host_port}: {banner!r}")
        except Exception as ex:  # pylint: disable=broad-except
            last_error = ex
        time.sleep(poll_interval)
    raise TimeoutError(
        f"VM sshd never served SSH protocol banner on port {host_port} within {total_timeout}s: {last_error}"
    )


def _wait_for_boot_marker(console, timeout: int):
    """Block until the guest prints :data:`BOOT_COMPLETE_MARKER` on its serial console.

    This is the primary boot gate and it costs the guest nothing: probing sshd to find out
    whether sshd is up is self-defeating, because every failed probe occupies a pre-auth
    connection slot and pushes a merely slow guest towards refusing connections outright.
    The marker is printed only after sshd has bound port 22, so one SSH check suffices
    afterwards instead of a poll loop.
    """
    if console is None:
        return
    if not console.line_reader.read_until(BOOT_COMPLETE_MARKER, timeout=timeout):
        raise TimeoutError(f"Guest never reported '{BOOT_COMPLETE_MARKER}' on serial within {timeout}s")


def execute_async_with_retries(target, binary_path, attempts: int = 3, ssh_recovery_timeout_s: int = 30, **kwargs):
    """Launch ``binary_path`` on ``target``, retrying if the SSH session collapses.

    ``QemuTarget.execute_async`` opens a *brand-new* SSH connection per launch, and this guest
    can refuse or drop one shortly after serving another (see ``_wait_for_ssh``); in CI that
    surfaces as ``SSH connection ... failed`` or ``EOFError`` from ``exec_command``. Both abort
    before the remote shell reports its PID, so there is no process handle left to reclaim and
    waiting for sshd to settle before dialling again is the cheapest recovery.

    It also opens its channel without a timeout, so it is wrapped in the same watchdog as the
    readiness probe to keep a wedged guest from stalling the test until bazel kills it.
    """
    last_error = None
    for attempt in range(1, attempts + 1):
        if attempt > 1:
            try:
                _wait_for_ssh(target, total_timeout=ssh_recovery_timeout_s, stable_successes=2)
            except Exception as probe_error:  # pylint: disable=broad-except
                logger.warning("VM still not serving SSH before retry %d (%s)", attempt, probe_error)
        try:
            return _call_with_watchdog(lambda: target.execute_async(binary_path, **kwargs), _LAUNCH_TIMEOUT_S)
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
        boot_timeout=120,
        ssh_timeout=60,
        cpu=None,
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
            cpu=cpu,
        )
        self._vm_config = vm_config
        self._max_boot_attempts = max_boot_attempts
        self._boot_timeout = boot_timeout
        self._ssh_timeout = ssh_timeout
        self._target = None

    def start(self):
        """Boot the VM, retrying up to ``max_boot_attempts`` times if it never becomes usable."""
        last_error = None
        for attempt in range(1, self._max_boot_attempts + 1):
            super().start()
            try:
                _wait_for_boot_marker(self.console, self._boot_timeout)
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
        """Restart the VM, reusing ``start()``'s boot-retry and readiness wait."""
        logger.warning("VM went unresponsive; restarting to self-heal")
        self.stop()
        self.start()

    @property
    def target(self):
        """The ``QemuTarget`` for this VM (available after ``start()``)."""
        return self._target
