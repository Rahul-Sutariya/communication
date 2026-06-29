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

#ifndef SCORE_QUALITY_INTEGRATION_TESTING_DUAL_QEMU_IVSHMEM_TYPED_MEMORY_H
#define SCORE_QUALITY_INTEGRATION_TESTING_DUAL_QEMU_IVSHMEM_TYPED_MEMORY_H

// A reusable building block for running the *production* lib/memory stack
// (score::memory::shared) over a cross-VM QEMU ivshmem region.
//
// score::memory::shared::SharedMemoryFactory can place a shared-memory object into "typed
// memory" instead of an ordinary /dev/shmem object, by delegating the allocation to a
// score::memory::shared::TypedMemory provider that the application injects via
// SharedMemoryFactory::SetTypedMemoryProvider(). The stock provider talks to the proprietary
// QNX typedmemd daemon, which is not available in this environment.
//
// BarTypedMemory is a drop-in TypedMemory provider that instead backs every named
// shared-memory object with one fixed physical range -- the ivshmem PCI BAR -- using the QNX
// shm_ctl(SHMCTL_PHYS) mechanism. After AllocateNamedTypedMemory() has bound the name to the
// BAR, the factory's own shm_open()+mmap() of that name lands directly on the BAR, so
// OffsetPtr / SharedMemoryResource / MemoryResourceRegistry all operate over memory that is
// physically shared between the two VMs.
//
// Typical usage (see the gateway integration test):
//
//     std::uint64_t paddr = 0, size = 0;
//     if (!DiscoverIvshmemBar(paddr, size)) { /* handle error */ }
//     auto provider = std::make_shared<BarTypedMemory>(paddr, size);
//     SharedMemoryFactory::SetTypedMemoryProvider(provider);
//     auto resource = SharedMemoryFactory::Create("/lola_ivshmem", bytes, init_cb,
//                                                  permissions, /*prefer_typed_memory=*/true);

// typed_memory.h transitively provides user_permission.h (permission::UserPermissions),
// score/os/errno.h (score::os::Error) and score/expected.hpp (score::cpp::expected).
#include "score/memory/shared/typedshm/typedshm_wrapper/typed_memory.h"

#include <sys/types.h>  // uid_t

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace score::quality::dual_qemu
{

/// \brief Discover the QEMU ivshmem-plain shared-memory BAR through the QNX pci-server.
///
/// Finds the ivshmem device (vendor 0x1af4, device 0x1110), enables Memory-Space and
/// Bus-Master decoding on it (required because a `-kernel` boot skips the BIOS PCI
/// enumeration that would normally turn the device on), and returns the physical address and
/// size of its largest memory BAR (the shared RAM window).
///
/// \param bar_paddr  Out: physical base address of the shared-memory BAR.
/// \param bar_size   Out: size of the shared-memory BAR in bytes.
/// \return true on success; false off QNX or if the device / a memory BAR cannot be found.
bool DiscoverIvshmemBar(std::uint64_t& bar_paddr, std::uint64_t& bar_size) noexcept;

/// \brief A score::memory::shared::TypedMemory provider backed by a fixed physical range.
///
/// Every named shared-memory object created through this provider is bound to the same
/// physical range -- the ivshmem BAR passed to the constructor -- via shm_ctl(SHMCTL_PHYS).
/// This lets SharedMemoryFactory::Create(..., prefer_typed_memory=true) place the LoLa
/// data-plane shared memory directly on the cross-VM BAR.
class BarTypedMemory final : public score::memory::shared::TypedMemory
{
  public:
    /// \param bar_paddr  Physical base address of the ivshmem BAR (from DiscoverIvshmemBar).
    /// \param bar_size   Size of the ivshmem BAR in bytes.
    BarTypedMemory(std::uint64_t bar_paddr, std::uint64_t bar_size) noexcept;

    /// Create a named shm object and bind its backing store to the ivshmem BAR. The factory
    /// subsequently re-opens the name and mmap()s it, so the mapping lands on the BAR.
    score::cpp::expected_blank<score::os::Error> AllocateNamedTypedMemory(
        const std::size_t shm_size,
        const std::string shm_name,
        const score::memory::shared::permission::UserPermissions& permissions) const noexcept override;

    /// Anonymous typed memory is not supported by this BAR-backed provider.
    score::cpp::expected<int, score::os::Error> AllocateAndOpenAnonymousTypedMemory(
        const std::uint64_t shm_size) const noexcept override;

    /// Remove a previously created named shm object.
    score::cpp::expected_blank<score::os::Error> Unlink(std::string_view shm_name) const noexcept override;

    /// Return the uid that created the named object (here: the calling process' uid).
    score::cpp::expected<uid_t, score::os::Error> GetCreatorUid(std::string_view shm_name) const noexcept override;

  private:
    std::uint64_t bar_paddr_;
    std::uint64_t bar_size_;
};

}  // namespace score::quality::dual_qemu

#endif  // SCORE_QUALITY_INTEGRATION_TESTING_DUAL_QEMU_IVSHMEM_TYPED_MEMORY_H
