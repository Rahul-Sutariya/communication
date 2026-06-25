# `dual_qemu` ITF plugin

A pytest/ITF plugin that boots **two** QNX QEMU VMs which share a single QEMU
[`ivshmem`](https://www.qemu.org/docs/master/system/devices/ivshmem.html) region. It is a
thin extension of the upstream single-VM `qemu` plugin
(`@score_itf//score/itf/plugins/qemu`).

## What it adds over the upstream `qemu` plugin

| Aspect            | upstream `qemu`        | `dual_qemu`                                            |
| ----------------- | ---------------------- | ----------------------------------------------------- |
| Number of VMs     | 1 (`target_init`)      | 2 (`target_a`, `target_b`)                            |
| Shared memory     | none                   | one `ivshmem-plain` region backed by one host file    |
| SSH ports         | one                    | distinct host port per VM (e.g. `2222` / `2223`)      |
| Inter-VM NIC      | n/a                    | optional point-to-point socket NIC (off by default)   |

The shared region uses **`ivshmem-plain`** — a passive shared-memory PCI device with
**no MSI-X and no doorbell**. Cross-VM notifications travel over a separate
socket-based control plane, so the data plane only needs plain shared memory.

## Fixtures

| Fixture            | Scope   | Description                                              |
| ------------------ | ------- | ------------------------------------------------------- |
| `target_a`         | session | First VM (`QemuTarget`).                                 |
| `target_b`         | session | Second VM (`QemuTarget`).                                |
| `ivshmem_backend`  | session | Path of the shared host backing file.                   |
| `config`           | session | Loaded `DualQemuConfigModel` + `qemu_image`.            |

Both VMs run the upstream `pre_tests_phase` checks (ping / SSH / SFTP) before the tests
start.

### Boot reliability

Booting two QNX guests **concurrently** under KVM occasionally wedges the second guest
during device bring-up (it can hang at SMP AP / secondary-CPU startup under the `-kernel`
boot path, so its `sshd` never comes up and QEMU's SLIRP resets the harness connections).
To keep the test reliable the plugin:

- boots the VMs **sequentially** — it starts a VM, waits until SSH is *stably* reachable
  and runs `pre_tests_phase`, and only then starts the next one, so the two guests never
  initialise their devices at the same time;
- gives each VM a single core and a **distinct NIC MAC**;
- the `dual_qemu_integration_test` macro additionally marks the test `flaky = True` so
  bazel transparently retries the rare residual KVM boot hiccup.

## CLI options

| Option               | Required | Description                                        |
| -------------------- | -------- | -------------------------------------------------- |
| `--dual-qemu-config` | yes      | JSON config (see `dual_qemu_config.example.json`). |
| `--qemu-image`       | yes      | Shared QEMU IFS image booted by both VMs.          |

## Configuration

See [`dual_qemu_config.example.json`](dual_qemu_config.example.json). A config is just
**two ordinary QEMU configs** (each an upstream `QemuConfigModel`) plus an `ivshmem`
block:

```json
{
    "ivshmem": { "size": "4M" },
    "intervm_network": { "enabled": false, "host_port": 12345 },
    "vms": [ { "...": "QemuConfigModel for VM-A" },
             { "...": "QemuConfigModel for VM-B" } ]
}
```

- `ivshmem.size` — size of the shared region (e.g. `4M`).
- `ivshmem.mem_path` (optional) — explicit host backing file; when empty a temporary file
  is created for the session and removed on teardown.
- `intervm_network.enabled` — when `true`, adds a socket NIC where VM-A listens and VM-B
  connects on `host_port` (for the future socket control plane).
- Each VM's `ssh_port` and `port_forwarding` must use **distinct host ports**.

## Files

| File                              | Purpose                                                        |
| --------------------------------- | -------------------------------------------------------------- |
| `__init__.py`                     | `pytest_addoption` + fixtures (`target_a`/`target_b`/...).      |
| `config.py`                       | `DualQemuConfigModel` (two `QemuConfigModel` + `ivshmem`).      |
| `ivshmem_qemu.py`                 | QEMU launcher that appends the `ivshmem-plain` device.         |
| `dual_qemu_process.py`            | Process wrapper around `IvshmemQemu`.                           |
| `BUILD`                           | `py_library` + `py_itf_plugin` (`:dual_qemu_plugin`).          |
| `dual_qemu_config.example.json`   | Sample two-VM configuration.                                   |
| `launch_dual_qemu.sh`             | Launch both VMs by hand for manual SSH / experimentation.      |

## Using it from a test

Wire `@//quality/integration_testing/environments/dual_qemu:dual_qemu_plugin` as the
plugin of a `py_itf_test`-based target (instead of the single-VM `qemu_plugin`) and pass
`--dual-qemu-config` / `--qemu-image`. The easiest way is the
`dual_qemu_integration_test` macro in
`//quality/integration_testing:integration_testing.bzl`, which builds the shared IFS image
and wires everything for you.

## Simple C++ example

[`example/`](example) contains the smallest possible cross-VM demo:

- [`example/simple_ivshmem.cpp`](example/simple_ivshmem.cpp) — one C++ binary that either
  `--write`s a string into the shared `ivshmem` region or `--read`s it back. On QNX it
  reaches the region through `pci-server` (the default, `--pci`) by mapping the ivshmem
  device's shared-memory BAR directly — no `/dev/ivshmem0` driver needed. No mw::com involved.
- [`example/simple_ivshmem_test.py`](example/simple_ivshmem_test.py) — runs the writer on
  `target_a` (VM-A) and the reader on `target_b` (VM-B).
- [`example/BUILD`](example/BUILD) — builds the binary, packages it, and runs it via
  `dual_qemu_integration_test`.

### How to see both VMs' results

Run the test with streamed (or `all`) output so the per-VM `print` lines are shown:

```bash
bazel test //quality/integration_testing/environments/dual_qemu/example:test_simple_ivshmem \
    --config=qnx_x86_64 \
    --test_output=streamed
```

You will see both results, for example:

```text
[VM-A] VM-A wrote 19 bytes: "HELLO_FROM_VM_A BMW" (rc=0)
[VM-B] VM-B read 19 bytes: "HELLO_FROM_VM_A BMW" (rc=0)
```

The test then asserts VM-B read back exactly what VM-A wrote.

> Note: on QNX the example maps the ivshmem PCI device's shared-memory BAR via `pci-server`
> (`--pci`), so it needs no `/dev/ivshmem0` guest driver. The same C++ logic can be
> smoke-tested on the host against any regular 4 KiB file via `--path <file>`.

## Running it manually

[`launch_dual_qemu.sh`](launch_dual_qemu.sh) boots the same two ivshmem-sharing VMs by
hand so you can SSH in and drive the example yourself.

1. Build the IFS image (contains the `simple_ivshmem` app + `sshd`):

   ```bash
   bazel build --config=qnx_x86_64 \
     //quality/integration_testing/environments/dual_qemu/example:_init_ifs_test_simple_ivshmem
   ```

2. Start both VMs (from the repo root):

   ```bash
   ./quality/integration_testing/environments/dual_qemu/launch_dual_qemu.sh
   ```

3. In two other terminals, SSH in (the `root` password is empty):

   ```bash
   ssh -p 2222 root@localhost      # VM-A
   ssh -p 2223 root@localhost      # VM-B
   ```

4. Write from VM-A and read it back on VM-B:

   ```bash
   # on VM-A:
   /opt/simple_ivshmem/bin/simple_ivshmem --write "HELLO_FROM_VM_A" --pci
   # on VM-B:
   /opt/simple_ivshmem/bin/simple_ivshmem --read --pci
   ```

5. Stop the VMs:

   ```bash
   ./quality/integration_testing/environments/dual_qemu/launch_dual_qemu.sh stop
   ```

The script honours environment overrides such as `IMG`, `SHM_PATH`, `SHM_SIZE`, `RAM`,
`CORES`, `CPU`, `SSH_PORT_A`, `SSH_PORT_B` and `RUN_DIR`; serial logs are written under
`RUN_DIR` (default `/tmp/dual_qemu`).

## Debugging guest boot

Set `DUAL_QEMU_SERIAL_DIR` to capture each VM's serial console to
`<dir>/vm<index>_serial.log` (instead of the normal `mon:stdio`). For example:

```bash
bazel test //quality/integration_testing/environments/dual_qemu/example:test_simple_ivshmem \
    --config=qnx_x86_64 --strategy=TestRunner=local \
    --test_env=DUAL_QEMU_SERIAL_DIR=/tmp/dqdbg
# then inspect /tmp/dqdbg/vm0_serial.log and /tmp/dqdbg/vm1_serial.log
```

