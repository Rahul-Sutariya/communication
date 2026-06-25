/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/

// The simplest possible cross-VM shared-memory example.
//
//   simple_ivshmem --write "hello"   // run on VM-A: publishes a string
//   simple_ivshmem --read            // run on VM-B: reads it back
//
// Both invocations map the SAME shared region: the QEMU ivshmem-plain PCI device that
// both VMs back with one host file. VM-A stores a string and then publishes a "magic"
// marker with a release store; VM-B waits for that marker with an acquire load and
// prints the string. No mw::com / LoLa types are involved -- this only demonstrates the
// raw shared-memory data plane between the two VMs.
//
// How the region is reached:
//   * On QNX (the default, --pci) the program talks to pci-server, finds the ivshmem
//     device (vendor 0x1af4, device 0x1110), and maps its shared-memory BAR directly.
//     No /dev/ivshmem0 driver is required.
//   * With --path <dev|file> it instead open()/mmap()s a device node or plain file,
//     which is handy for a host-side smoke test (e.g. --path /dev/shm/score_ivshmem).

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(__QNXNTO__)
// The QNX PCI server header declares plain C functions but does not guard them with
// extern "C", so wrap it to avoid C++ name mangling when linking against libpci.
extern "C" {
#include <pci/pci.h>
}
#include <sys/neutrino.h>  // ThreadCtl, _NTO_TCTL_IO
#endif

namespace
{
constexpr std::size_t kMapSize = 4096U;        // we only need the first page of the region
constexpr std::uint32_t kMagic = 0x53484D31U;  // "SHM1": set by the writer once data is ready

// Layout placed at the very start of the shared region. Both VMs agree on it.
struct SharedBlock
{
    std::uint32_t magic;
    std::uint32_t length;
    char data[256];
};

// A mapped view of the shared region plus the bookkeeping needed to release it.
struct Mapping
{
    void* addr = nullptr;
    std::size_t size = 0U;
    int fd = -1;                 // >= 0 for the open()/mmap() path
    bool device_memory = false;  // true for the QNX mmap_device_memory() path
};

void Unmap(Mapping& m)
{
    if (m.addr == nullptr)
    {
        return;
    }
#if defined(__QNXNTO__)
    if (m.device_memory)
    {
        ::munmap_device_memory(m.addr, m.size);
    }
    else
#endif
    {
        ::munmap(m.addr, m.size);
    }
    if (m.fd >= 0)
    {
        ::close(m.fd);
    }
    m = Mapping{};
}

// Maps a device node or plain file (host smoke test, or a guest with an ivshmem driver).
bool MapFromPath(const std::string& path, Mapping& out)
{
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0)
    {
        std::perror("open");
        return false;
    }

    void* const addr = ::mmap(nullptr, kMapSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
    {
        std::perror("mmap");
        ::close(fd);
        return false;
    }

    out.addr = addr;
    out.size = kMapSize;
    out.fd = fd;
    out.device_memory = false;
    return true;
}

#if defined(__QNXNTO__)
// Finds the QEMU ivshmem-plain device through pci-server and maps its shared-memory BAR.
bool MapFromPci(Mapping& out)
{
    constexpr pci_vid_t kVendor = 0x1af4U;  // Red Hat / virtio vendor used by ivshmem
    constexpr pci_did_t kDevice = 0x1110U;  // ivshmem device id

    // Reading/writing PCI configuration space from a user process is a privileged operation
    // on QNX. Without I/O privilege the in-process config access faults ("Memory fault").
    // ThreadCtl(_NTO_TCTL_IO) requests it; it succeeds because the example runs as root
    // (which holds PROCMGR_AID_IO).
    if (::ThreadCtl(_NTO_TCTL_IO, nullptr) == -1)
    {
        std::perror("ThreadCtl(_NTO_TCTL_IO)");
        return false;
    }

    const pci_bdf_t bdf = pci_device_find(0U, kVendor, kDevice, PCI_CCODE_ANY);
    if (bdf == PCI_BDF_NONE)
    {
        std::fprintf(stderr, "ivshmem PCI device %04x:%04x not found (is pci-server running?)\n", kVendor, kDevice);
        return false;
    }

    pci_err_t err = PCI_ERR_OK;
    const pci_devhdl_t hdl = pci_device_attach(bdf, pci_attachFlags_OWNER, &err);
    if (hdl == nullptr || err != PCI_ERR_OK)
    {
        std::fprintf(stderr, "pci_device_attach failed (err=%d)\n", static_cast<int>(err));
        return false;
    }

    // Booting the QNX IFS directly with `-kernel` skips the BIOS PCI enumeration that would
    // normally turn on the device. pci-server assigns the BAR addresses but leaves memory
    // decoding off, so enable Memory Space (and Bus Master) ourselves -- otherwise the CPU
    // accesses to the mapped BAR are not decoded and fault ("Memory fault").
    constexpr pci_cmd_t kMemSpaceEnable = static_cast<pci_cmd_t>(1U << 1);
    constexpr pci_cmd_t kBusMasterEnable = static_cast<pci_cmd_t>(1U << 2);
    pci_cmd_t cmd = 0U;
    err = pci_device_read_cmd(bdf, &cmd);
    if (err != PCI_ERR_OK)
    {
        std::fprintf(stderr, "pci_device_read_cmd failed (err=%d)\n", static_cast<int>(err));
        (void)pci_device_detach(hdl);
        return false;
    }
    pci_cmd_t cmd_set = 0U;
    err = pci_device_write_cmd(hdl, static_cast<pci_cmd_t>(cmd | kMemSpaceEnable | kBusMasterEnable), &cmd_set);
    if (err != PCI_ERR_OK)
    {
        std::fprintf(stderr, "pci_device_write_cmd failed (err=%d)\n", static_cast<int>(err));
        (void)pci_device_detach(hdl);
        return false;
    }

    pci_ba_t ba[7];
    int_t nba = static_cast<int_t>(sizeof(ba) / sizeof(ba[0]));
    err = pci_device_read_ba(hdl, &nba, ba, pci_reqType_e_UNSPECIFIED);
    if (err != PCI_ERR_OK)
    {
        std::fprintf(stderr, "pci_device_read_ba failed (err=%d)\n", static_cast<int>(err));
        (void)pci_device_detach(hdl);
        return false;
    }

    // The shared-memory BAR is the largest memory region (BAR0 holds the small register
    // block, the shared RAM lives in BAR2).
    const pci_ba_t* shared = nullptr;
    for (int_t i = 0; i < nba; ++i)
    {
        if (ba[i].type == pci_asType_e_MEM && (shared == nullptr || ba[i].size > shared->size))
        {
            shared = &ba[i];
        }
    }
    if (shared == nullptr)
    {
        std::fprintf(stderr, "no memory BAR found on the ivshmem device\n");
        (void)pci_device_detach(hdl);
        return false;
    }

    // One concise line describing the region we are about to map.
    std::fprintf(stderr,
                 "ivshmem: BAR%d addr=0x%llx size=0x%llx\n",
                 static_cast<int>(shared->bar_num),
                 static_cast<unsigned long long>(shared->addr),
                 static_cast<unsigned long long>(shared->size));

    void* const addr = ::mmap_device_memory(nullptr, shared->size, PROT_READ | PROT_WRITE, 0, shared->addr);
    if (addr == MAP_FAILED)
    {
        std::perror("mmap_device_memory");
        (void)pci_device_detach(hdl);
        return false;
    }

    // Keep the device attached for the lifetime of the process; the OS releases it on exit
    // and the mapping remains valid until Unmap().
    out.addr = addr;
    out.size = static_cast<std::size_t>(shared->size);
    out.fd = -1;
    out.device_memory = true;
    return true;
}
#endif  // __QNXNTO__
}  // namespace

int main(int argc, char** argv)
{
    std::string mode;
    std::string path;
    std::string message;
    bool use_pci = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--write" && (i + 1) < argc)
        {
            mode = "write";
            message = argv[++i];
        }
        else if (arg == "--read")
        {
            mode = "read";
        }
        else if (arg == "--pci")
        {
            use_pci = true;
        }
        else if (arg == "--path" && (i + 1) < argc)
        {
            path = argv[++i];
        }
        else
        {
            std::fprintf(stderr, "usage: %s --write <message> | --read [--pci | --path <dev|file>]\n", argv[0]);
            return 2;
        }
    }

    if (mode.empty())
    {
        std::fprintf(stderr, "usage: %s --write <message> | --read [--pci | --path <dev|file>]\n", argv[0]);
        return 2;
    }

    // Default to PCI discovery on QNX when no explicit path was given.
#if defined(__QNXNTO__)
    if (path.empty())
    {
        use_pci = true;
    }
#endif

    if (!use_pci && path.empty())
    {
        std::fprintf(stderr, "error: specify --pci or --path <dev|file>\n");
        return 2;
    }

    Mapping mapping;
    bool mapped = false;
    if (use_pci)
    {
#if defined(__QNXNTO__)
        mapped = MapFromPci(mapping);
#else
        std::fprintf(stderr, "error: --pci is only supported on QNX; use --path instead\n");
        return 1;
#endif
    }
    else
    {
        mapped = MapFromPath(path, mapping);
    }

    if (!mapped)
    {
        return 1;
    }

    auto* const block = static_cast<SharedBlock*>(mapping.addr);
    int result = 0;

    if (mode == "write")
    {
        const std::size_t length = std::min(message.size(), sizeof(block->data) - 1U);
        std::memcpy(block->data, message.data(), length);
        block->data[length] = '\0';
        block->length = static_cast<std::uint32_t>(length);
        // Publish the data: the release store guarantees the bytes above are visible to a
        // reader that observes the magic with an acquire load.
        __atomic_store_n(&block->magic, kMagic, __ATOMIC_RELEASE);
        std::printf("VM-A wrote %zu bytes: \"%s\"\n", length, block->data);
    }
    else  // read
    {
        if (__atomic_load_n(&block->magic, __ATOMIC_ACQUIRE) != kMagic)
        {
            std::fprintf(stderr, "VM-B: no data published yet (magic mismatch)\n");
            result = 3;
        }
        else
        {
            std::printf("VM-B read %u bytes: \"%s\"\n", block->length, block->data);
        }
    }

    Unmap(mapping);
    return result;
}
