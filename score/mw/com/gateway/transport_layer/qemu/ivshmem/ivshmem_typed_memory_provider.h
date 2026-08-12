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

#if defined(__QNXNTO__)
#include "score/os/qnx/mman.h"
#include "score/os/qnx/mman_impl.h"
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace score::mw::com::gateway::qemu::ivshmem
{

/// @brief A TypedMemory provider that backs named shm objects with sub-ranges of the ivshmem BAR.
///
/// This provider supports multiple shm objects (DATA + CTRL for multiple services) by using a
/// BAR-resident allocation directory that is visible to both VMs via the shared physical BAR memory.
///
/// @details
/// **BAR Layout** (managed by the provider):
/// - [0 .. usable_end): shm data regions (allocated by either VM)
/// - [usable_end .. usable_end + kDirectorySize): allocation directory (shared, both VMs)
///
/// Both VMs share a single allocation space with no partitioning. A shared lock in the
/// directory serializes cross-VM access so the directory acts as a shared allocator:
/// - **Source side** (AllocateNamedTypedMemory): scans all directory entries to find the
///   next free offset (after the highest existing allocation), writes a new entry, and
///   binds the QNX shm object to that BAR sub-range.
/// - **Destination side** (AllocateNamedTypedMemoryAtOffset): binds a QNX shm object at a
///   specific offset (looked up from the directory by the transport layer).
///
/// This design supports any number of providers and consumers on both VMs simultaneously,
/// with services being created and consumed in any order.
class IvshmemTypedMemoryProvider : public score::memory::shared::TypedMemory
{
  public:
    /// @brief Constructs an IvshmemTypedMemoryProvider.
    ///
    /// @param paddr    Physical base address of the ivshmem BAR (from DiscoverIvshmemBar).
    /// @param size     Total usable BAR size in bytes (excluding any handshake region managed outside).
    /// @param mman_qnx QNX mman abstraction (only on QNX platforms).
#if defined(__QNXNTO__)
    IvshmemTypedMemoryProvider(
        std::uint64_t paddr,
        std::uint64_t size,
        std::unique_ptr<score::os::qnx::MmanQnx> mman_qnx = std::make_unique<score::os::qnx::MmanQnxImpl>()) noexcept;
#else
    IvshmemTypedMemoryProvider(std::uint64_t paddr, std::uint64_t size) noexcept;
#endif
    ~IvshmemTypedMemoryProvider() override = default;

    /// @brief Allocates a page-aligned sub-region from the BAR.
    ///
    /// Scans the BAR-resident directory (visible to both VMs) to find the next free offset,
    /// writes a new directory entry, and binds the QNX shm to that sub-range.
    ///
    /// @param shm_size   Size of the shared memory region to allocate.
    /// @param shm_name   Name of the shared memory object.
    /// @param permissions User permissions for the shared memory.
    ///
    /// @return Expected value containing no error on success, or a score::os::Error on failure.
    ///
    score::cpp::expected_blank<score::os::Error> AllocateNamedTypedMemory(
        std::size_t shm_size,
        std::string shm_name,
        const score::memory::shared::permission::UserPermissions& permissions) const noexcept override;

    /// @brief Binds a named shm at a specific BAR offset.
    ///
    /// The offset is looked up from the directory by the transport layer (LookupOffsetInDirectory).
    ///
    /// @param shm_size   Size of the shared memory region to allocate.
    /// @param shm_name   Name of the shared memory object.
    /// @param bar_offset Offset within the BAR where the shm should be bound.
    /// @param permissions User permissions for the shared memory.
    ///
    /// @return Expected value containing no error on success, or a score::os::Error on failure.
    ///
    virtual score::cpp::expected_blank<score::os::Error> AllocateNamedTypedMemoryAtOffset(
        std::size_t shm_size,
        const std::string& shm_name,
        std::uint64_t bar_offset,
        const score::memory::shared::permission::UserPermissions& permissions) const noexcept;

    /// @brief Returns the BAR offset for a previously allocated name (local process lookup).
    ///
    /// @param shm_name Name of the shared memory object.
    ///
    /// @return The BAR offset if found, or std::nullopt if not found.
    ///
    std::optional<std::uint64_t> GetAllocationOffset(const std::string& shm_name) const noexcept;

    /// @brief Looks up a shm name in the BAR-resident directory (cross-VM lookup).
    ///
    /// Reads from shared physical memory so it works even if the name was allocated by the
    /// other VM. Returns the offset if found.
    ///
    /// @param shm_name Name of the shared memory object to look up.
    ///
    /// @return The BAR offset if found in the directory, or std::nullopt if not found.
    ///
    virtual std::optional<std::uint64_t> LookupOffsetInDirectory(const std::string& shm_name) const noexcept;

    /// @brief Allocates and opens an anonymous typed shared memory region.
    ///
    /// @param shm_size Size of the shared memory region in bytes.
    ///
    /// @return Expected value containing the file descriptor on success, or a score::os::Error on failure.
    score::cpp::expected<int, score::os::Error> AllocateAndOpenAnonymousTypedMemory(
        std::uint64_t shm_size) const noexcept override;

    /// @brief Unlinks a shared memory object.
    ///
    /// @param shm_name Name of the shared memory object to unlink.
    ///
    /// @return Expected value containing no error on success, or a score::os::Error on failure.
    score::cpp::expected_blank<score::os::Error> Unlink(std::string_view shm_name) const noexcept override;

    /// @brief Gets the user ID (UID) of the creator of a shared memory object.
    ///
    /// @param shm_name Name of the shared memory object.
    ///
    /// @return Expected value containing the UID on success, or a score::os::Error on failure.
    score::cpp::expected<uid_t, score::os::Error> GetCreatorUid(std::string_view shm_name) const noexcept override;

    /// @brief Directory entry stored in the BAR.
    ///
    /// Visible to both VMs via the shared physical memory. Used by the allocation directory
    /// to track allocated regions.
    struct DirectoryEntry
    {
        std::uint32_t name_hash;   ///< FNV-1a hash of the shm name
        std::uint32_t alloc_size;  ///< page-aligned allocation size in bytes
        std::uint64_t bar_offset;  ///< offset within the usable BAR region
    };

    /// @brief Size of the directory region reserved at the end of the usable BAR.
    static constexpr std::uint64_t kDirectorySize = 4096U;

    /// @brief Size of the directory header (cross-VM lock + entry count).
    static constexpr std::uint64_t kDirectoryHeaderSize = sizeof(std::uint32_t) * 2U;

    /// @brief Maximum directory entries (header + entries fit in one page).
    static constexpr std::uint32_t kMaxDirectoryEntries =
        (kDirectorySize - kDirectoryHeaderSize) / sizeof(DirectoryEntry);

    /// FNV-1a 32-bit hash algorithm offset basis
    static constexpr std::uint32_t kFnvOffsetBasis = 2166136261U;

    /// FNV-1a 32-bit hash algorithm prime
    static constexpr std::uint32_t kFnvPrime = 16777619U;

    /// @brief Computes FNV-1a 32-bit hash for a string.
    ///
    /// @param name String to hash.
    ///
    /// @return The 32-bit FNV-1a hash value.
    static std::uint32_t HashName(const std::string& name) noexcept;

  protected:
    /// @brief Creates the QNX shm object bound to BAR at (paddr_ + offset).
    ///
    /// @param shm_size Size of the shared memory region in bytes.
    /// @param shm_name Name of the shared memory object.
    /// @param offset   Offset within the BAR where the shm should be bound.
    ///
    /// @return Expected value containing no error on success, or a score::os::Error on failure.
    ///
    score::cpp::expected_blank<score::os::Error> BindShmToBar(std::size_t shm_size,
                                                              const std::string& shm_name,
                                                              std::uint64_t offset) const noexcept;

    /// @brief Local cache: name -> BAR offset.
    ///
    /// Protected to allow testing via subclass.
    mutable std::unordered_map<std::string, std::uint64_t> allocations_;

  private:
#if defined(__QNXNTO__)
    /// @brief Writes an entry to the BAR-resident directory.
    ///
    /// @param shm_name Name of the shared memory object.
    /// @param offset   Offset within the BAR where the shm is bound.
    /// @param size     Size of the shared memory allocation.
    void WriteDirectoryEntry(const std::string& shm_name, std::uint64_t offset, std::uint32_t size) const noexcept;

    /// @brief Scans all directory entries (from both VMs) and returns the offset just past the highest existing
    /// allocation.
    ///
    /// This is the next free offset for a new allocation.
    ///
    /// @return The next available offset for a new allocation.
    ///
    std::uint64_t FindNextFreeOffsetInDirectory() const noexcept;

    /// @brief Maps the directory region of the BAR into this process's address space (cached).
    ///
    /// @return Pointer to the mapped directory region, or nullptr on failure.
    void* MapDirectory() const noexcept;

    mutable void* directory_map_{nullptr};  ///< cached mmap of the directory region
    std::unique_ptr<score::os::qnx::MmanQnx> mman_qnx_;
    std::uint64_t paddr_;        ///< Physical base address of the ivshmem BAR
    std::uint64_t usable_size_;  ///< size - kDirectorySize (space for shm allocations)
#endif

    mutable std::mutex mutex_;  ///< protects the local per-process allocation cache
};

}  // namespace score::mw::com::gateway::qemu::ivshmem

#endif  // SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_QEMU_IVSHMEM_TYPED_MEMORY_PROVIDER_H
