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
#include "score/mw/com/gateway/transport_layer/qemu/ivshmem/ivshmem_typed_memory_provider.h"
#include "score/mw/log/logging.h"

#include "score/os/mman.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#if defined(__QNXNTO__)
#include <sys/mman.h>
#if defined(__X86_64__) || defined(__x86_64__)
#include <x86_64/mmu.h>
#endif
#endif

namespace score::mw::com::gateway::qemu::ivshmem
{

namespace
{
#if defined(__QNXNTO__)
constexpr std::uint64_t kPageSize = 4096U;

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) noexcept
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}
#endif
}  // namespace

#if defined(__QNXNTO__)
IvshmemTypedMemoryProvider::IvshmemTypedMemoryProvider(std::uint64_t paddr,
                                                       std::uint64_t size,
                                                       std::unique_ptr<score::os::qnx::MmanQnx> mman_qnx) noexcept
    : paddr_{paddr}, mman_qnx_{std::move(mman_qnx)}, usable_size_{size > kDirectorySize ? size - kDirectorySize : 0U}
{
}
#else
IvshmemTypedMemoryProvider::IvshmemTypedMemoryProvider(std::uint64_t /*paddr*/, std::uint64_t /*size*/) noexcept {}
#endif

std::uint32_t IvshmemTypedMemoryProvider::HashName(const std::string& name) noexcept
{
    // FNV-1a 32-bit
    std::uint32_t hash = kFnvOffsetBasis;
    for (const char c : name)
    {
        hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        hash *= kFnvPrime;
    }
    return hash;
}

#if defined(__QNXNTO__)
void* IvshmemTypedMemoryProvider::MapDirectory() const noexcept
{
    if (directory_map_ != nullptr)
    {
        return directory_map_;
    }
    const std::uint64_t dir_paddr = paddr_ + usable_size_;
    // MAP_PHYS|MAP_SHARED with fd=NOFD maps physical memory — equivalent to mmap_device_memory().
    const auto mmap_result = mman_qnx_->mmap(nullptr,
                                             kDirectorySize,
                                             static_cast<std::int32_t>(PROT_READ | PROT_WRITE),
                                             static_cast<std::int32_t>(MAP_PHYS | MAP_SHARED),
                                             static_cast<std::int32_t>(NOFD),
                                             static_cast<std::int64_t>(dir_paddr));
    if (!mmap_result.has_value())
    {
        return nullptr;
    }
    directory_map_ = mmap_result.value();
    return mmap_result.value();
}
#endif

#if defined(__QNXNTO__)
void IvshmemTypedMemoryProvider::WriteDirectoryEntry(const std::string& shm_name,
                                                     std::uint64_t offset,
                                                     std::uint32_t size) const noexcept
{
    void* dir = MapDirectory();
    if (dir == nullptr)
    {
        return;
    }

    auto* count_ptr = static_cast<volatile std::uint32_t*>(dir);
    auto* entries = reinterpret_cast<volatile DirectoryEntry*>(static_cast<char*>(dir) + sizeof(std::uint32_t));

    const std::uint32_t count = *count_ptr;
    if (count >= kMaxDirectoryEntries)
    {
        ::score::mw::log::LogError() << "IvshmemTypedMemoryProvider: directory full (" << count << " entries)";
        return;
    }

    const std::uint32_t hash = HashName(shm_name);

    // Idempotent: check if already recorded. Only reachable in a multi-VM concurrent
    // write race; the allocations_ cache prevents re-entry within a single VM.
    // COV_JUSTIFIED_START ivshmem-write-directory-entry-idempotent
    for (std::uint32_t i = 0U; i < count; ++i)
    {
        if (entries[i].name_hash == hash && entries[i].bar_offset == offset)
        {
            return;
        }
    }
    // COV_JUSTIFIED_STOP

    // Append new entry, then increment count with release semantics.
    DirectoryEntry entry{};
    entry.name_hash = hash;
    entry.alloc_size = size;
    entry.bar_offset = offset;

    std::memcpy(const_cast<DirectoryEntry*>(&entries[count]), &entry, sizeof(entry));
    std::atomic_thread_fence(std::memory_order_release);
    *count_ptr = count + 1U;
}

std::uint64_t IvshmemTypedMemoryProvider::FindNextFreeOffsetInDirectory() const noexcept
{
    void* dir = MapDirectory();
    if (dir == nullptr)
    {
        return 0U;
    }

    auto* count_ptr = static_cast<volatile std::uint32_t*>(dir);
    auto* entries = reinterpret_cast<volatile DirectoryEntry*>(static_cast<char*>(dir) + sizeof(std::uint32_t));

    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint32_t count = *count_ptr;

    // Find the end of the highest existing allocation (from either VM).
    std::uint64_t highest_end = 0U;
    for (std::uint32_t i = 0U; i < count && i < kMaxDirectoryEntries; ++i)
    {
        const std::uint64_t entry_end = entries[i].bar_offset + entries[i].alloc_size;
        highest_end = std::max(highest_end, entry_end);
    }

    return AlignUp(highest_end, kPageSize);
}
#endif

std::optional<std::uint64_t> IvshmemTypedMemoryProvider::LookupOffsetInDirectory(
    const std::string& shm_name) const noexcept
{
#if defined(__QNXNTO__)
    void* dir = MapDirectory();
    if (dir == nullptr)
    {
        return std::nullopt;
    }

    auto* count_ptr = static_cast<volatile std::uint32_t*>(dir);
    auto* entries = reinterpret_cast<volatile DirectoryEntry*>(static_cast<char*>(dir) + sizeof(std::uint32_t));

    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint32_t count = *count_ptr;
    const std::uint32_t hash = HashName(shm_name);

    for (std::uint32_t i = 0U; i < count && i < kMaxDirectoryEntries; ++i)
    {
        if (entries[i].name_hash == hash)
        {
            return static_cast<std::uint64_t>(entries[i].bar_offset);
        }
    }
    return std::nullopt;
#else
    (void)shm_name;
    return std::nullopt;
#endif
}

score::cpp::expected_blank<score::os::Error> IvshmemTypedMemoryProvider::BindShmToBar(
    [[maybe_unused]] std::size_t shm_size,
    [[maybe_unused]] const std::string& shm_name,
    [[maybe_unused]] std::uint64_t offset) const noexcept
{
#if defined(__QNXNTO__)
    const std::uint64_t alloc_size = AlignUp(static_cast<std::uint64_t>(shm_size), kPageSize);
    const std::uint64_t sub_paddr = paddr_ + offset;

    const auto shm_open_result = mman_qnx_->shm_open(shm_name.c_str(), O_RDWR | O_CREAT, mode_t{0666});
    if (!shm_open_result.has_value())
    {
        return score::cpp::make_unexpected(shm_open_result.error());
    }
    const std::int32_t fd = shm_open_result.value();

    // Request Write-Back caching (ivshmem-plain is host-RAM backed, WB is correct).
    // shm_ctl_special succeeds only on real ivshmem hardware with a WB-capable host;
    // on x86_64 test VMs it always returns -1 (EBADF with a negative test fd).
#if defined(__X86_64__) || defined(__x86_64__)
    // COV_JUSTIFIED_START ivshmem-bind-shm-to-bar-shm-ctl-special-success
    const int rc_special = ::shm_ctl_special(
        fd, SHMCTL_PHYS | SHMCTL_HAS_SPECIAL, sub_paddr, alloc_size, static_cast<unsigned>(X86_64_PTE_MTYPE_WB));
    if (rc_special != -1)
    {
        std::ignore = ::close(static_cast<int>(fd));
        return {};
    }
    // COV_JUSTIFIED_STOP
#endif
    const auto shm_ctl_result = mman_qnx_->shm_ctl(fd, SHMCTL_PHYS, sub_paddr, alloc_size);
    if (!shm_ctl_result.has_value())
    {
        const score::os::Error saved_error = shm_ctl_result.error();
        std::ignore = ::close(static_cast<int>(fd));
        score::cpp::ignore = score::os::Mman::instance().shm_unlink(shm_name.c_str());
        return score::cpp::make_unexpected(saved_error);
    }

    std::ignore = ::close(static_cast<int>(fd));
    return {};
#else
    return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENOSYS));
#endif
}

score::cpp::expected_blank<score::os::Error> IvshmemTypedMemoryProvider::AllocateNamedTypedMemory(
    [[maybe_unused]] std::size_t shm_size,
    [[maybe_unused]] std::string shm_name,
    const score::memory::shared::permission::UserPermissions& /*permissions*/) const noexcept
{
#if defined(__QNXNTO__)
    std::lock_guard<std::mutex> lock{mutex_};

    // Check local cache first — if already allocated by this process, reuse.
    auto it = allocations_.find(shm_name);
    if (it != allocations_.end())
    {
        return BindShmToBar(shm_size, shm_name, it->second);
    }

    // Check if already in the BAR directory (e.g. allocated by the other VM, or by a
    // previous run of this process). Reuse the same offset.
    auto dir_offset = LookupOffsetInDirectory(shm_name);
    if (dir_offset.has_value())
    {
        allocations_[shm_name] = dir_offset.value();
        return BindShmToBar(shm_size, shm_name, dir_offset.value());
    }

    // New allocation: scan ALL directory entries (from both VMs) to find the next free
    // offset after the highest existing allocation.
    const std::uint64_t offset = FindNextFreeOffsetInDirectory();
    const std::uint64_t alloc_size = AlignUp(static_cast<std::uint64_t>(shm_size), kPageSize);

    if (offset + alloc_size > usable_size_)
    {
        ::score::mw::log::LogError() << "IvshmemTypedMemoryProvider: BAR exhausted (need "
                                     << static_cast<std::uint64_t>(alloc_size) << " at offset "
                                     << static_cast<std::uint64_t>(offset) << ", usable "
                                     << static_cast<std::uint64_t>(usable_size_) << ")";
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENOMEM));
    }

    // Write to the BAR-resident directory so the other VM can discover this allocation.
    WriteDirectoryEntry(shm_name, offset, static_cast<std::uint32_t>(alloc_size));
    allocations_[shm_name] = offset;

    return BindShmToBar(shm_size, shm_name, offset);
#else
    return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENOSYS));
#endif
}

score::cpp::expected_blank<score::os::Error> IvshmemTypedMemoryProvider::AllocateNamedTypedMemoryAtOffset(
    [[maybe_unused]] std::size_t shm_size,
    [[maybe_unused]] const std::string& shm_name,
    [[maybe_unused]] std::uint64_t bar_offset,
    const score::memory::shared::permission::UserPermissions& /*permissions*/) const noexcept
{
#if defined(__QNXNTO__)
    std::lock_guard<std::mutex> lock{mutex_};

    const std::uint64_t alloc_size = AlignUp(static_cast<std::uint64_t>(shm_size), kPageSize);
    if (bar_offset + alloc_size > usable_size_)
    {
        ::score::mw::log::LogError() << "IvshmemTypedMemoryProvider::AtOffset: offset "
                                     << static_cast<std::uint64_t>(bar_offset) << " + size "
                                     << static_cast<std::uint64_t>(alloc_size) << " exceeds usable BAR ("
                                     << static_cast<std::uint64_t>(usable_size_) << ")";
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENOMEM));
    }

    // Record locally (or verify consistency if already known).
    auto it = allocations_.find(shm_name);
    if (it != allocations_.end())
    {
        if (it->second != bar_offset)
        {
            ::score::mw::log::LogError() << "IvshmemTypedMemoryProvider::AtOffset: name '" << shm_name
                                         << "' already at offset " << static_cast<std::uint64_t>(it->second)
                                         << ", requested " << static_cast<std::uint64_t>(bar_offset);
            return score::cpp::make_unexpected(score::os::Error::createFromErrno(EEXIST));
        }
    }
    else
    {
        allocations_[shm_name] = bar_offset;
    }

    return BindShmToBar(shm_size, shm_name, bar_offset);
#else
    return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENOSYS));
#endif
}

std::optional<std::uint64_t> IvshmemTypedMemoryProvider::GetAllocationOffset(const std::string& shm_name) const noexcept
{
    std::lock_guard<std::mutex> lock{mutex_};
    auto it = allocations_.find(shm_name);
    if (it != allocations_.end())
    {
        return it->second;
    }
    return std::nullopt;
}

score::cpp::expected<int, score::os::Error> IvshmemTypedMemoryProvider::AllocateAndOpenAnonymousTypedMemory(
    std::uint64_t /*shm_size*/) const noexcept
{
    return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENOSYS));
}

score::cpp::expected_blank<score::os::Error> IvshmemTypedMemoryProvider::Unlink(
    std::string_view shm_name) const noexcept
{
    const std::string name{shm_name};
    const auto result = score::os::Mman::instance().shm_unlink(name.c_str());
    if (!result.has_value())
    {
        return score::cpp::make_unexpected(result.error());
    }
    return {};
}

score::cpp::expected<uid_t, score::os::Error> IvshmemTypedMemoryProvider::GetCreatorUid(
    std::string_view /*shm_name*/) const noexcept
{
    return ::getuid();
}

}  // namespace score::mw::com::gateway::qemu::ivshmem
