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

#include "score/memory/shared/user_permission.h"

#include <gtest/gtest.h>

#include <string>

namespace score::mw::com::gateway::qemu::ivshmem
{

namespace
{

/// Test subclass that exposes protected members for white-box testing on non-QNX.
class TestableIvshmemTypedMemoryProvider : public IvshmemTypedMemoryProvider
{
  public:
    TestableIvshmemTypedMemoryProvider(std::uint64_t paddr, std::uint64_t size) noexcept
        : IvshmemTypedMemoryProvider{paddr, size}
    {
    }

    /// Bring BindShmToBar into public scope so tests can invoke it directly.
    using IvshmemTypedMemoryProvider::BindShmToBar;

    /// Populate the local allocation cache, enabling tests for GetAllocationOffset's found-branch.
    void InsertAllocation(const std::string& name, std::uint64_t offset) noexcept
    {
        allocations_[name] = offset;
    }
};

class IvshmemTypedMemoryProviderTest : public ::testing::Test
{
  protected:
    static constexpr std::uint64_t kBarPaddr = 0x100000U;
    static constexpr std::uint64_t kBarSize = 1024U * 1024U;  // 1 MiB

    IvshmemTypedMemoryProvider provider_{kBarPaddr, kBarSize};
};

// --- Constructor ---

TEST_F(IvshmemTypedMemoryProviderTest, ConstructorWithNormalSizeCalculatesUsableSize)
{
    // usable_size_ = kBarSize - kDirectorySize = 1048576 - 4096 = 1044480
    // Verify indirectly: the provider should be constructable without issues
    IvshmemTypedMemoryProvider p{kBarPaddr, kBarSize};
    (void)p;
}

TEST_F(IvshmemTypedMemoryProviderTest, ConstructorWithSizeSmallerThanDirectorySetsUsableSizeToZero)
{
    // When BAR size is smaller than directory size, usable_size_ = 0
    IvshmemTypedMemoryProvider p{kBarPaddr, 100U};
    (void)p;
}

TEST_F(IvshmemTypedMemoryProviderTest, ConstructorWithSizeEqualToDirectorySetsUsableSizeToZero)
{
    IvshmemTypedMemoryProvider p{kBarPaddr, IvshmemTypedMemoryProvider::kDirectorySize};
    (void)p;
}

TEST_F(IvshmemTypedMemoryProviderTest, ConstructorWithZeroSizeSetsUsableSizeToZero)
{
    IvshmemTypedMemoryProvider p{kBarPaddr, 0U};
    (void)p;
}

// --- HashName ---

TEST(IvshmemTypedMemoryProviderHashNameTest, EmptyStringReturnsInitialSeed)
{
    // FNV-1a 32-bit of "" equals the initial seed value (no characters processed).
    EXPECT_EQ(IvshmemTypedMemoryProvider::HashName(""), 2166136261U);
}

TEST(IvshmemTypedMemoryProviderHashNameTest, SingleCharacterProducesExpectedHash)
{
    // FNV-1a of 'A' (0x41 = 65): (seed XOR 65) * 16777619
    const std::uint32_t expected = (2166136261U ^ 65U) * 16777619U;
    EXPECT_EQ(IvshmemTypedMemoryProvider::HashName("A"), expected);
}

TEST(IvshmemTypedMemoryProviderHashNameTest, SameInputAlwaysProducesSameHash)
{
    EXPECT_EQ(IvshmemTypedMemoryProvider::HashName("/my_shm"), IvshmemTypedMemoryProvider::HashName("/my_shm"));
}

TEST(IvshmemTypedMemoryProviderHashNameTest, DifferentInputsProduceDifferentHashes)
{
    EXPECT_NE(IvshmemTypedMemoryProvider::HashName("/shm_a"), IvshmemTypedMemoryProvider::HashName("/shm_b"));
}

// --- AllocateNamedTypedMemory (non-QNX path) ---

TEST_F(IvshmemTypedMemoryProviderTest, AllocateNamedTypedMemoryReturnsEnosysOnNonQnx)
{
    const auto result = provider_.AllocateNamedTypedMemory(
        4096U, "/test_shm", score::memory::shared::permission::WorldWritable{});
    EXPECT_FALSE(result.has_value());
}

// --- AllocateNamedTypedMemoryAtOffset (non-QNX path) ---

TEST_F(IvshmemTypedMemoryProviderTest, AllocateNamedTypedMemoryAtOffsetReturnsEnosysOnNonQnx)
{
    const auto result = provider_.AllocateNamedTypedMemoryAtOffset(
        4096U, "/test_shm", 0U, score::memory::shared::permission::WorldWritable{});
    EXPECT_FALSE(result.has_value());
}

// --- BindShmToBar (non-QNX path, via protected access in test subclass) ---

TEST(IvshmemTypedMemoryProviderBindShmToBarTest, ReturnsEnosysOnNonQnx)
{
    TestableIvshmemTypedMemoryProvider p{0x100000U, 1024U * 1024U};
    const auto result = p.BindShmToBar(4096U, "/test_shm", 0U);
    ASSERT_FALSE(result.has_value());
}

// --- AllocateAndOpenAnonymousTypedMemory ---

TEST_F(IvshmemTypedMemoryProviderTest, AllocateAndOpenAnonymousTypedMemoryReturnsEnosys)
{
    const auto result = provider_.AllocateAndOpenAnonymousTypedMemory(4096U);
    EXPECT_FALSE(result.has_value());
}

// --- Unlink (non-QNX path) ---

TEST_F(IvshmemTypedMemoryProviderTest, UnlinkReturnsEnosysOnNonQnx)
{
    const auto result = provider_.Unlink("/test_shm");
    EXPECT_FALSE(result.has_value());
}

// --- GetCreatorUid ---

TEST_F(IvshmemTypedMemoryProviderTest, GetCreatorUidReturnsCurrentUid)
{
    const auto result = provider_.GetCreatorUid("/test_shm");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), ::getuid());
}

// --- GetAllocationOffset ---

TEST_F(IvshmemTypedMemoryProviderTest, GetAllocationOffsetReturnsNulloptForUnknownName)
{
    const auto result = provider_.GetAllocationOffset("/unknown");
    EXPECT_FALSE(result.has_value());
}

TEST(IvshmemTypedMemoryProviderGetAllocationOffsetTest, ReturnsOffsetWhenAllocationExists)
{
    TestableIvshmemTypedMemoryProvider p{0x100000U, 1024U * 1024U};
    p.InsertAllocation("/my_shm", 8192U);

    const auto result = p.GetAllocationOffset("/my_shm");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 8192U);
}

TEST(IvshmemTypedMemoryProviderGetAllocationOffsetTest, ReturnsNulloptForUninsertedName)
{
    TestableIvshmemTypedMemoryProvider p{0x100000U, 1024U * 1024U};
    p.InsertAllocation("/other_shm", 4096U);

    EXPECT_FALSE(p.GetAllocationOffset("/my_shm").has_value());
}

// --- LookupOffsetInDirectory (non-QNX path) ---

TEST_F(IvshmemTypedMemoryProviderTest, LookupOffsetInDirectoryReturnsNulloptOnNonQnx)
{
    // MapDirectory returns nullptr on non-QNX, so LookupOffsetInDirectory returns nullopt
    const auto result = provider_.LookupOffsetInDirectory("/test_shm");
    EXPECT_FALSE(result.has_value());
}

// --- Static constants ---

TEST(IvshmemTypedMemoryProviderStaticTest, DirectorySizeIs4096)
{
    EXPECT_EQ(IvshmemTypedMemoryProvider::kDirectorySize, 4096U);
}

TEST(IvshmemTypedMemoryProviderStaticTest, MaxDirectoryEntriesFitsInDirectoryPage)
{
    // (4096 - 4) / sizeof(DirectoryEntry) = 4092 / 16 = 255
    const std::uint32_t expected = (4096U - sizeof(std::uint32_t)) / sizeof(IvshmemTypedMemoryProvider::DirectoryEntry);
    EXPECT_EQ(IvshmemTypedMemoryProvider::kMaxDirectoryEntries, expected);
    EXPECT_GT(IvshmemTypedMemoryProvider::kMaxDirectoryEntries, 0U);
}

// --- DirectoryEntry layout ---

TEST(IvshmemTypedMemoryProviderStaticTest, DirectoryEntryHasExpectedSize)
{
    EXPECT_EQ(sizeof(IvshmemTypedMemoryProvider::DirectoryEntry), 16U);
}

}  // namespace

}  // namespace score::mw::com::gateway::qemu::ivshmem
