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

// The simplest possible cross-VM shared-memory DATA-PLANE example.
//
//   simple_ivshmem --id A    // VM-A: writes its own list, then reads + verifies VM-B's
//   simple_ivshmem --id B    // VM-B: writes its own list, then reads + verifies VM-A's
//
// By default each VM does BOTH in a single run: it writes an offset-pointer linked list
// into its own slot and then waits for the peer's slot to appear and verifies it. The two
// VMs therefore rendezvous, so they must run concurrently (each one waits for the other).
// You can still split the phases with --write / --read if you prefer to run them by hand:
//
//   simple_ivshmem --id A --write    // only write slot A
//   simple_ivshmem --id B --read     // only read + verify slot A (peer of B)
//
// Both VMs map the SAME shared region (the QEMU ivshmem-plain PCI device that both VMs
// back with one host file) but, crucially, at DIFFERENT virtual base addresses. The whole
// point of this example is the LoLa data-plane property: a pointer stored INSIDE the shared
// region must resolve on the other VM despite that different base. A raw `T*` cannot do
// this -- it is only valid in the writer's address space. So instead of a flat string we
// store a singly-linked list whose links are self-relative byte offsets ("offset pointers",
// exactly like score::memory::shared::OffsetPtr). Each VM writes its OWN slot and reads the
// PEER slot, then walks the list purely through offset pointers and verifies the values.
// Each run prints the base address it mapped the region at, so the logs show the two VMs
// resolving the same data structure from different bases.
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

#include <cstdint>
#include <cstdio>
#include <ctime>
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
constexpr std::size_t kMapSize = 4096U;        // one page is plenty for the small layout below
constexpr std::uint32_t kMagic = 0x53484D31U;  // "SHM1": published once a slot is fully written
constexpr std::uint32_t kNodeCount = 8U;       // nodes per linked list
constexpr std::uint32_t kBaseValue[2] = {0x1000U, 0x2000U};  // per-VM base so values are distinguishable

// A node of a singly-linked list that lives ENTIRELY inside the shared region. The link is
// not a raw `Node*` (that would be a VM-local address, invalid on the other VM) but a
// self-relative byte offset -- an offset pointer, just like score::memory::shared::OffsetPtr.
struct Node
{
    std::uint32_t value;
    std::int64_t next_off;  // offset from &next_off to the next Node; 0 marks the end of the list
};

// One slot per VM. The owning VM writes its slot; the reader on the OTHER VM walks it.
struct Slot
{
    std::uint32_t magic;       // published LAST with a release store once the list is complete
    std::uint32_t node_count;  // number of nodes in the list
    std::int64_t head_off;     // offset from &head_off to the first Node (an offset pointer)
    Node nodes[kNodeCount];    // node storage, also inside the shared region
};

// The whole shared region: slot[0] belongs to VM-A, slot[1] to VM-B. Both VMs agree on it.
struct Region
{
    Slot slot[2];
};

// Store a self-relative offset: the byte distance from the offset field itself to `target`
// (or 0 for a null offset pointer). This is exactly how an offset pointer works, and it is
// what lets a pointer stored in shared memory resolve correctly even though VM-A and VM-B
// map the region at different virtual base addresses.
void StoreOffset(std::int64_t& field, const void* target)
{
    field = (target == nullptr)
                ? 0
                : reinterpret_cast<const std::uint8_t*>(target) - reinterpret_cast<std::uint8_t*>(&field);
}

// Resolve an offset pointer back into a usable address in THIS process's mapping.
void* LoadOffset(const std::int64_t& field)
{
    if (field == 0)
    {
        return nullptr;  // null offset pointer (end of list)
    }
    const std::uint8_t* const base = reinterpret_cast<const std::uint8_t*>(&field);
    return const_cast<std::uint8_t*>(base) + field;
}

// Wait (up to timeout_ms) until the peer publishes its slot, i.e. its magic becomes kMagic.
// Returns true if the magic was observed. This lets both VMs run as a single concurrent
// write-then-read without a strict start order: whichever VM gets to the read phase first
// simply waits for the other to publish.
bool WaitForMagic(const std::uint32_t& magic, std::uint32_t timeout_ms)
{
    constexpr std::uint32_t kStepMs = 20U;
    for (std::uint32_t waited = 0U;; waited += kStepMs)
    {
        if (__atomic_load_n(&magic, __ATOMIC_ACQUIRE) == kMagic)
        {
            return true;
        }
        if (waited >= timeout_ms)
        {
            return false;
        }
        const struct timespec ts{0, static_cast<long>(kStepMs) * 1000000L};
        (void)::nanosleep(&ts, nullptr);
    }
}

// Build this VM's linked list inside its own slot and publish it. Every node and every link
// lives in the shared region, and the links are offset pointers, so the peer can walk the
// list at its own mapping base.
void WriteOwnSlot(Region& region, int id, const char* name, const void* base)
{
    Slot& slot = region.slot[id];
    for (std::uint32_t i = 0; i < kNodeCount; ++i)
    {
        slot.nodes[i].value = kBaseValue[id] + i;
        StoreOffset(slot.nodes[i].next_off, (i + 1U < kNodeCount) ? &slot.nodes[i + 1U] : nullptr);
    }
    slot.node_count = kNodeCount;
    StoreOffset(slot.head_off, &slot.nodes[0]);
    // Release store: makes the nodes + offsets above visible to a peer that reads the magic
    // with an acquire load.
    __atomic_store_n(&slot.magic, kMagic, __ATOMIC_RELEASE);
    std::printf("%s wrote %u-node offset-pointer list into slot %d (region mapped at %p)\n",
                name, kNodeCount, id, base);
}

// Wait for the peer to publish its slot, then walk its list through offset pointers and
// verify the values. Returns a process exit code (0 = OK).
int ReadPeerSlot(Region& region, int id, const char* name, const void* base)
{
    const int peer = 1 - id;
    Slot& slot = region.slot[peer];

    constexpr std::uint32_t kWaitMs = 15000U;
    if (!WaitForMagic(slot.magic, kWaitMs))
    {
        std::fprintf(stderr, "%s: peer slot %d not published within %ums\n", name, peer, kWaitMs);
        return 3;
    }

    // Because head_off/next_off are self-relative, this resolves correctly even though we
    // mapped the region at a DIFFERENT base than the writer did.
    std::printf("%s reading peer slot %d (region mapped at %p):", name, peer, base);
    std::uint32_t seen = 0U;
    bool ok = true;
    for (auto* node = static_cast<Node*>(LoadOffset(slot.head_off));
         node != nullptr && seen <= kNodeCount;  // the bound also guards against a cyclic list
         node = static_cast<Node*>(LoadOffset(node->next_off)))
    {
        std::printf(" %u", node->value);
        ok = ok && (node->value == kBaseValue[peer] + seen);
        ++seen;
    }
    std::printf("\n");

    if (!ok || seen != slot.node_count)
    {
        std::fprintf(stderr, "%s: list verification FAILED (seen=%u, node_count=%u)\n", name, seen, slot.node_count);
        return 4;
    }
    std::printf("%s verified %u nodes via offset pointers OK\n", name, seen);
    return 0;
}

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

// Maps the shared region: via the QNX ivshmem PCI device (--pci) or an mmap'd file (--path).
bool MapShared(bool use_pci, const std::string& path, Mapping& out)
{
    if (use_pci)
    {
#if defined(__QNXNTO__)
        return MapFromPci(out);
#else
        std::fprintf(stderr, "error: --pci is only supported on QNX; use --path instead\n");
        return false;
#endif
    }
    return MapFromPath(path, out);
}

// Command-line options.
struct Options
{
    int id = -1;            // 0 = VM-A, 1 = VM-B
    std::string path;       // set by --path
    bool use_pci = false;   // set by --pci
    bool want_write = false;
    bool want_read = false;
};

// Parses argv into opts. Returns false on error or a missing/invalid --id.
bool ParseArgs(int argc, char** argv, Options& opts)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--write")
        {
            opts.want_write = true;
        }
        else if (arg == "--read")
        {
            opts.want_read = true;
        }
        else if (arg == "--pci")
        {
            opts.use_pci = true;
        }
        else if (arg == "--id" && (i + 1) < argc)
        {
            const std::string v = argv[++i];
            opts.id = (v == "A" || v == "a") ? 0 : ((v == "B" || v == "b") ? 1 : -1);
        }
        else if (arg == "--path" && (i + 1) < argc)
        {
            opts.path = argv[++i];
        }
        else
        {
            return false;
        }
    }
    return opts.id >= 0;
}
}  // namespace

int main(int argc, char** argv)
{
    Options opts;
    if (!ParseArgs(argc, argv, opts))
    {
        std::fprintf(stderr, "usage: %s --id <A|B> [--write] [--read] [--pci | --path <dev|file>]\n", argv[0]);
        return 2;
    }

    // Default: do BOTH in one run -- write our own slot, then read + verify the peer's.
    if (!opts.want_write && !opts.want_read)
    {
        opts.want_write = true;
        opts.want_read = true;
    }

    // On QNX default to PCI discovery when no explicit path was given.
#if defined(__QNXNTO__)
    if (opts.path.empty())
    {
        opts.use_pci = true;
    }
#endif
    if (!opts.use_pci && opts.path.empty())
    {
        std::fprintf(stderr, "error: specify --pci or --path <dev|file>\n");
        return 2;
    }

    Mapping mapping;
    if (!MapShared(opts.use_pci, opts.path, mapping))
    {
        return 1;
    }
    if (mapping.size < sizeof(Region))
    {
        std::fprintf(stderr, "shared region too small: %zu < %zu\n", mapping.size, sizeof(Region));
        Unmap(mapping);
        return 1;
    }

    auto& region = *static_cast<Region*>(mapping.addr);
    const char* const name = (opts.id == 0) ? "VM-A" : "VM-B";

    int result = 0;
    if (opts.want_write)
    {
        WriteOwnSlot(region, opts.id, name, mapping.addr);
    }
    if (opts.want_read)
    {
        result = ReadPeerSlot(region, opts.id, name, mapping.addr);
    }

    Unmap(mapping);
    return result;
}
