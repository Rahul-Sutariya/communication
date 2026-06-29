/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 ********************************************************************************/

// Stage 1 smoke test for running the PRODUCTION lib/memory stack over the ivshmem BAR.
//
//   lola_ivshmem
//
// This is the single-VM, full-stack proof that BarTypedMemory works end to end:
//
//   1. DiscoverIvshmemBar()                         -- find the ivshmem BAR via pci-server
//   2. SharedMemoryFactory::SetTypedMemoryProvider() -- inject our BAR-backed provider
//   3. SharedMemoryFactory::Create(..., prefer_typed_memory=true)
//        -> the provider binds the named shm object to the BAR with shm_ctl(SHMCTL_PHYS),
//           then the factory shm_open()+mmap()s it, so the whole lib/memory region lives on
//           the BAR.
//   4. In the init callback we build a singly-linked list whose links are real
//        score::memory::shared::OffsetPtr<Node> (the LoLa data-plane pointer), allocated
//        through the resource (resource->construct<T>()).
//   5. After Create() we walk the list back through OffsetPtr and verify the values.
//
// THE decisive check is resource->IsShmInTypedMemory(): if shm_ctl(SHMCTL_PHYS) is rejected,
// the factory SILENTLY falls back to ordinary /dev/shmem (NOT the BAR), so without this
// assertion the test would "pass" while proving nothing. We therefore fail hard if the
// object did not end up in typed memory.
//
// QNX only (the provider and pci discovery are QNX-specific).

#include "quality/integration_testing/environments/dual_qemu/example/ivshmem_typed_memory.h"

#include "score/memory/shared/i_shared_memory_resource.h"
#include "score/memory/shared/offset_ptr.h"
#include "score/memory/shared/shared_memory_factory.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace
{
using score::memory::shared::ISharedMemoryResource;
using score::memory::shared::OffsetPtr;
using score::memory::shared::SharedMemoryFactory;

constexpr std::uint32_t kMagic = 0x4C4F4C41U;  // "LOLA": published once the list is complete
constexpr std::uint32_t kNodeCount = 8U;
constexpr std::uint32_t kBaseValue = 0x1000U;

// A node living ENTIRELY inside the shared region; the link is a real OffsetPtr (self-relative),
// so it resolves regardless of the mapping base -- exactly the LoLa data-plane property.
struct Node
{
    std::uint32_t value;
    OffsetPtr<Node> next;
};

// The root object placed at the start of the user region. magic is published LAST.
struct Root
{
    std::uint32_t magic;
    std::uint32_t count;
    OffsetPtr<Node> head;
};
}  // namespace

int main()
{
    std::uint64_t bar_paddr = 0U;
    std::uint64_t bar_size = 0U;
    if (!score::quality::dual_qemu::DiscoverIvshmemBar(bar_paddr, bar_size))
    {
        std::fprintf(stderr, "FAILED: could not discover the ivshmem BAR\n");
        return 1;
    }

    // Inject the BAR-backed TypedMemory provider so Create(prefer_typed_memory=true) lands on it.
    auto provider = std::make_shared<score::quality::dual_qemu::BarTypedMemory>(bar_paddr, bar_size);
    SharedMemoryFactory::SetTypedMemoryProvider(provider);

    const std::string path = "/lola_ivshmem";
    // Clean up any object left over from a previous run so Create() starts fresh.
    SharedMemoryFactory::RemoveStaleArtefacts(path);

    // The init callback builds the OffsetPtr list. We capture the root pointer so we can walk
    // the list afterwards without having to recompute the (alignment-padded) usable address.
    Root* root = nullptr;
    auto resource = SharedMemoryFactory::Create(
        path,
        [&root](std::shared_ptr<ISharedMemoryResource> res) {
            root = res->construct<Root>();
            root->magic = 0U;
            root->count = kNodeCount;
            root->head = nullptr;

            Node* prev = nullptr;
            for (std::uint32_t i = 0U; i < kNodeCount; ++i)
            {
                Node* const node = res->construct<Node>();
                node->value = kBaseValue + i;
                node->next = nullptr;
                if (prev == nullptr)
                {
                    root->head = node;  // first node becomes the list head
                }
                else
                {
                    prev->next = node;
                }
                prev = node;
            }
            // Release store: publish the fully built list.
            __atomic_store_n(&root->magic, kMagic, __ATOMIC_RELEASE);
        },
        /*user_space_to_reserve=*/ 4096U,
        SharedMemoryFactory::WorldWritable{},
        /*prefer_typed_memory=*/ true);

    if (resource == nullptr)
    {
        std::fprintf(stderr, "FAILED: SharedMemoryFactory::Create returned nullptr\n");
        return 2;
    }

    // Decisive check: the object must really be in typed memory (i.e. on the BAR). If
    // shm_ctl(SHMCTL_PHYS) had been rejected, the factory would have fallen back to ordinary
    // /dev/shmem and this would be false.
    if (!resource->IsShmInTypedMemory())
    {
        std::fprintf(stderr,
                     "FAILED: object is NOT in typed memory -- shm_ctl(SHMCTL_PHYS) was likely rejected, "
                     "the factory fell back to /dev/shmem\n");
        return 3;
    }
    std::printf("Create OK in typed memory: usable base=%p, BAR paddr=0x%llx size=0x%llx\n",
                resource->getUsableBaseAddress(),
                static_cast<unsigned long long>(bar_paddr),
                static_cast<unsigned long long>(bar_size));

    // Walk the list back through OffsetPtr and verify.
    if (__atomic_load_n(&root->magic, __ATOMIC_ACQUIRE) != kMagic)
    {
        std::fprintf(stderr, "FAILED: root magic was not published\n");
        SharedMemoryFactory::Remove(path);
        return 4;
    }

    std::printf("walking OffsetPtr list:");
    std::uint32_t seen = 0U;
    bool ok = true;
    for (Node* node = root->head.get(); node != nullptr && seen <= kNodeCount; node = node->next.get())
    {
        std::printf(" %u", node->value);
        ok = ok && (node->value == kBaseValue + seen);
        ++seen;
    }
    std::printf("\n");

    SharedMemoryFactory::Remove(path);

    if (!ok || seen != kNodeCount)
    {
        std::fprintf(stderr, "FAILED: list verification (seen=%u, expected=%u)\n", seen, kNodeCount);
        return 5;
    }

    std::printf("OK: built + walked a %u-node OffsetPtr list over the ivshmem BAR via lib/memory\n", seen);
    return 0;
}
