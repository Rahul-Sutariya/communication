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

#include "quality/integration_testing/environments/dual_qemu/example/ivshmem_typed_memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

#if defined(__QNXNTO__)
// The QNX PCI server header declares plain C functions but does not guard them with
// extern "C", so wrap it to avoid C++ name mangling when linking against libpci.
extern "C" {
#include <pci/pci.h>
}
#include <sys/neutrino.h>  // ThreadCtl, _NTO_TCTL_IO
#endif

namespace score::quality::dual_qemu
{
namespace
{
score::os::Error ErrnoError(const int err) noexcept
{
    return score::os::Error::createFromErrno(err);
}
}  // namespace

bool DiscoverIvshmemBar(std::uint64_t& bar_paddr, std::uint64_t& bar_size) noexcept
{
#if defined(__QNXNTO__)
    constexpr pci_vid_t kVendor = 0x1af4U;  // Red Hat / virtio vendor used by ivshmem
    constexpr pci_did_t kDevice = 0x1110U;  // ivshmem device id

    // Reading/writing PCI configuration space from a user process is privileged on QNX.
    // ThreadCtl(_NTO_TCTL_IO) requests the I/O privilege (granted because we run as root).
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

    // A `-kernel` boot skips the BIOS PCI enumeration that would normally enable the device.
    // pci-server assigns the BAR addresses but leaves memory decoding off, so enable Memory
    // Space (and Bus Master) ourselves -- otherwise CPU accesses to the BAR are not decoded.
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

    std::fprintf(stderr,
                 "ivshmem: BAR%d addr=0x%llx size=0x%llx\n",
                 static_cast<int>(shared->bar_num),
                 static_cast<unsigned long long>(shared->addr),
                 static_cast<unsigned long long>(shared->size));

    bar_paddr = static_cast<std::uint64_t>(shared->addr);
    bar_size = static_cast<std::uint64_t>(shared->size);

    // Keep the device attached for the lifetime of the process; the OS releases it on exit.
    return true;
#else
    static_cast<void>(bar_paddr);
    static_cast<void>(bar_size);
    std::fprintf(stderr, "DiscoverIvshmemBar is only supported on QNX\n");
    return false;
#endif
}

BarTypedMemory::BarTypedMemory(std::uint64_t bar_paddr, std::uint64_t bar_size) noexcept
    : score::memory::shared::TypedMemory{}, bar_paddr_{bar_paddr}, bar_size_{bar_size}
{
}

score::cpp::expected_blank<score::os::Error> BarTypedMemory::AllocateNamedTypedMemory(
    const std::size_t shm_size,
    const std::string shm_name,
    const score::memory::shared::permission::UserPermissions& /*permissions*/) const noexcept
{
#if defined(__QNXNTO__)
    if (static_cast<std::uint64_t>(shm_size) > bar_size_)
    {
        // The requested object does not fit into the physical BAR window.
        return score::cpp::make_unexpected(ErrnoError(EFBIG));
    }

    // Create the named shm object. The factory will re-open this same name afterwards
    // (with O_RDWR | O_EXCL, no O_CREAT) and mmap() it; binding it to the BAR below makes
    // that mapping resolve to the cross-VM physical memory.
    const int fd = ::shm_open(shm_name.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    if (fd < 0)
    {
        return score::cpp::make_unexpected(ErrnoError(errno));
    }

    // Bind the object's backing store to the ivshmem BAR physical range. Both VMs run this
    // with the same BAR, so the same name resolves to the same physical memory on each VM.
    if (::shm_ctl(fd, SHMCTL_PHYS, bar_paddr_, bar_size_) == -1)
    {
        const int saved_errno = errno;
        (void)::close(fd);
        (void)::shm_unlink(shm_name.c_str());
        return score::cpp::make_unexpected(ErrnoError(saved_errno));
    }

    (void)::close(fd);
    return {};
#else
    static_cast<void>(shm_size);
    static_cast<void>(shm_name);
    return score::cpp::make_unexpected(ErrnoError(ENOSYS));
#endif
}

score::cpp::expected<int, score::os::Error> BarTypedMemory::AllocateAndOpenAnonymousTypedMemory(
    const std::uint64_t shm_size) const noexcept
{
    // This provider exposes one fixed, named physical window; anonymous allocation makes no
    // sense for it.
    static_cast<void>(shm_size);
    return score::cpp::make_unexpected(ErrnoError(ENOSYS));
}

score::cpp::expected_blank<score::os::Error> BarTypedMemory::Unlink(std::string_view shm_name) const noexcept
{
    const std::string name{shm_name};
    if (::shm_unlink(name.c_str()) == -1)
    {
        return score::cpp::make_unexpected(ErrnoError(errno));
    }
    return {};
}

score::cpp::expected<uid_t, score::os::Error> BarTypedMemory::GetCreatorUid(std::string_view shm_name) const noexcept
{
    static_cast<void>(shm_name);
    return ::getuid();
}

}  // namespace score::quality::dual_qemu
