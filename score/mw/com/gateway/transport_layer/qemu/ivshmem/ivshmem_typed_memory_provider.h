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
#ifndef SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_QEMU_IVSHMEM_TYPED_MEMORY_PROVIDER_H
#define SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_QEMU_IVSHMEM_TYPED_MEMORY_PROVIDER_H

#include "score/memory/shared/typedshm/typedshm_wrapper/typed_memory.h"

#include <score/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace score::mw::com::gateway::qemu::ivshmem
{

/// A TypedMemory provider that backs named shm objects with sub-ranges of the ivshmem BAR.
///
/// Supports multiple shm objects (DATA + CTRL for multiple services) by using a BAR-resident
/// allocation directory that is visible to both VMs via the shared physical BAR memory.
///
/// **BAR layout** (managed by the provider):
///   [0 .. usable_end)            — shm data regions (allocated by either VM)
///   [usable_end .. usable_end + kDirectorySize) — allocation directory (shared, both VMs)
///
/// Both VMs share a single allocation space with no partitioning. The directory acts as a
/// shared atomic allocator:
///   - **Source side** (AllocateNamedTypedMemory): scans all directory entries to find the
///     next free offset (after the highest existing allocation), writes a new entry, and
///     binds the QNX shm object to that BAR sub-range.
///   - **Destination side** (AllocateNamedTypedMemoryAtOffset): binds a QNX shm object at a
///     specific offset (looked up from the directory by the transport layer).
///
/// This design supports any number of providers and consumers on both VMs simultaneously,
/// with services being created and consumed in any order.
class IvshmemTypedMemoryProvider : public score::memory::shared::TypedMemory
{
  public:
    /// \param paddr  Physical base address of the ivshmem BAR (from DiscoverIvshmemBar).
    /// \param size   Total usable BAR size in bytes (excluding any handshake region managed outside).
    IvshmemTypedMemoryProvider(std::uint64_t paddr, std::uint64_t size) noexcept;
    ~IvshmemTypedMemoryProvider() override = default;

    /// Source side: allocates a page-aligned sub-region from the BAR.
    /// Scans the BAR-resident directory (visible to both VMs) to find the next free offset,
    /// writes a new directory entry, and binds the QNX shm to that sub-range.
    score::cpp::expected_blank<score::os::Error> AllocateNamedTypedMemory(
        std::size_t shm_size,
        std::string shm_name,
        const score::memory::shared::permission::UserPermissions& permissions) const noexcept override;

    /// Destination side: binds a named shm at a specific BAR offset.
    /// The offset is looked up from the directory by the transport layer (LookupOffsetInDirectory).
    virtual score::cpp::expected_blank<score::os::Error> AllocateNamedTypedMemoryAtOffset(
        std::size_t shm_size,
        std::string shm_name,
        std::uint64_t bar_offset,
        const score::memory::shared::permission::UserPermissions& permissions) const noexcept;

    /// Returns the BAR offset for a previously allocated name (local process lookup), or nullopt.
    std::optional<std::uint64_t> GetAllocationOffset(const std::string& shm_name) const noexcept;

    /// Looks up a shm name in the BAR-resident directory (cross-VM lookup).
    /// Reads from shared physical memory so it works even if the name was allocated by the
    /// other VM. Returns the offset if found.
    virtual std::optional<std::uint64_t> LookupOffsetInDirectory(const std::string& shm_name) const noexcept;

    score::cpp::expected<int, score::os::Error> AllocateAndOpenAnonymousTypedMemory(
        std::uint64_t shm_size) const noexcept override;

    score::cpp::expected_blank<score::os::Error> Unlink(std::string_view shm_name) const noexcept override;

    score::cpp::expected<uid_t, score::os::Error> GetCreatorUid(std::string_view shm_name) const noexcept override;

    /// Directory entry stored in the BAR. Visible to both VMs via the shared physical memory.
    struct DirectoryEntry
    {
        std::uint32_t name_hash;   // FNV-1a hash of the shm name
        std::uint32_t alloc_size;  // page-aligned allocation size in bytes
        std::uint64_t bar_offset;  // offset within the usable BAR region
    };

    /// Size of the directory region reserved at the end of the usable BAR.
    static constexpr std::uint64_t kDirectorySize = 4096U;
    /// Maximum directory entries (first 4 bytes = entry count, rest = entries).
    static constexpr std::uint32_t kMaxDirectoryEntries =
        (kDirectorySize - sizeof(std::uint32_t)) / sizeof(DirectoryEntry);

    /// Computes FNV-1a 32-bit hash for a string.
    static std::uint32_t HashName(const std::string& name) noexcept;

  protected:
    /// Creates the QNX shm object bound to BAR at (paddr_ + offset).
    score::cpp::expected_blank<score::os::Error> BindShmToBar(
        std::size_t shm_size,
        const std::string& shm_name,
        std::uint64_t offset) const noexcept;

    // Local cache: name -> BAR offset. Protected to allow testing via subclass.
    mutable std::unordered_map<std::string, std::uint64_t> allocations_;

  private:
#if defined(__QNXNTO__)
    /// Writes an entry to the BAR-resident directory.
    void WriteDirectoryEntry(const std::string& shm_name, std::uint64_t offset, std::uint32_t size) const noexcept;

    /// Scans all directory entries (from both VMs) and returns the offset just past the
    /// highest existing allocation. This is the next free offset for a new allocation.
    std::uint64_t FindNextFreeOffsetInDirectory() const noexcept;

    /// Maps the directory region of the BAR into this process's address space (cached).
    void* MapDirectory() const noexcept;

    mutable void* directory_map_{nullptr};  // cached mmap of the directory region
#endif

    std::uint64_t paddr_;
    std::uint64_t size_;              // total BAR size passed to constructor
    std::uint64_t usable_size_;       // size_ - kDirectorySize (space for shm allocations)
    mutable std::mutex mutex_;
};

}  // namespace score::mw::com::gateway::qemu::ivshmem

#endif  // SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_QEMU_IVSHMEM_TYPED_MEMORY_PROVIDER_H
