// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "bbport_copy.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    // bbport: arena pages are 4 GiB unless the driver's buffer size limit is lower (AMD's
    // Windows driver: 2 GiB); an arena spans up to two pages, so they shrink to half the limit.
    static constexpr u64 MAX_ARENA_PAGE_BITS = 32;
    static constexpr u64 MIN_ARENA_PAGE_BITS = 30;
    static constexpr u64 MAX_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - MIN_ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;

public:
    /// Read-only bindings up to this size are copied into a stream buffer (bbport: public for
    /// the draw pipeline's constant ring).
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    /// bbport: memory bound to the sparse arenas now (retired ranges excluded).
    u64 GetArenaResidentBytes() const noexcept {
        return arena_resident_bytes.load(std::memory_order_relaxed);
    }
    /// bbport: chunks backing the arenas: count, bytes, bytes of them in system memory.
    u32 GetArenaChunkCount() const noexcept {
        return arena_chunk_count.load(std::memory_order_relaxed);
    }
    u64 GetArenaChunkBytes() const noexcept {
        return arena_chunk_bytes.load(std::memory_order_relaxed);
    }
    u64 GetArenaSystemBytes() const noexcept {
        return arena_system_bytes.load(std::memory_order_relaxed);
    }
    /// bbport: arena upkeep since the last call (pressure report): bytes retired because no
    /// draw used them, unused bytes retired from system memory while VRAM had room (their next
    /// use lands in VRAM), chunks returned to VMA, and allocations that only succeeded after an
    /// emergency reclaim.
    struct ArenaReclaimStats {
        u64 reclaimed_bytes = 0;
        u64 migrated_bytes = 0;
        u32 released_chunks = 0;
        u32 emergencies = 0;
    };
    ArenaReclaimStats TakeArenaReclaimStats() noexcept {
        const ArenaReclaimStats stats = reclaim_stats;
        reclaim_stats = {};
        return stats;
    }

    /// bbport: VRAM levels the arena upkeep works against (from the texture cache's GC).
    struct MemoryLevels {
        u64 used = 0;     ///< driver usage minus the space free inside VMA blocks
        u64 budget = 0;   ///< 0: the driver reports none (no reclaim, no migration)
        u64 pressure = 0; ///< retire ranges unused for a while from here
        u64 critical = 0; ///< retire sooner, page-table ranges too, from here
    };
    /// bbport: once per submission, before its flush: releases ranges retired earlier whose
    /// submission completed, returns empty chunks to VMA, allocates the next VRAM chunk ahead
    /// of need, and retires ranges no draw used for seconds: under memory pressure, or in
    /// system memory while VRAM has room.
    void TickArenas(const MemoryLevels& levels);

    /// bbport: a shader read [address, address + size) through the page table (fault path).
    /// Such reads bind no descriptor and leave no trace of later use, so these ranges are
    /// retired only under critical pressure, and never moved.
    void NoteFaultedRange(VAddr address, u64 size);

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// bbport: a guest write fault at `device_addr` was handled; unprotect its neighbourhood.
    void ExtendWriteFault(VAddr device_addr);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// bbport: batched small copies on the copy threads (BbCopy::QueueCopy).
    static void RunGuestCopy(const BbCopy::Item& item);
    void SmallGuestCopy(const BbCopy::Item& item);

    const Buffer* UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                               size_t total_size_bytes);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    // bbport: texel buffer ranges known not to alias an image, per image registry generation.
    struct ImageMiss {
        VAddr address = 0;
        u32 size = 0;
        u64 generation = ~0ULL;
    };
    std::array<ImageMiss, 256> image_miss_cache{};
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;

    std::array<const Buffer*, MAX_ARENA_PAGES> address_space{};
    u64 arena_page_bits{MAX_ARENA_PAGE_BITS};
    u64 arena_page_size{u64{1} << MAX_ARENA_PAGE_BITS};
    u64 num_arena_pages{u64{1} << (ADDRESS_SPACE_BITS - MAX_ARENA_PAGE_BITS)};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    IntervalList<Backing> resident_ranges;

    u32 arena_memory_type_index{};
    /// bbport: system-memory type for arena blocks when VRAM runs out, ~0u if none fits.
    u32 arena_fallback_memory_type_index = ~0u;

    /// bbport: arena blocks come from 32 MiB VMA chunks (sharing VMA's blocks, and the space
    /// evicted images leave there), each with a bitmap of used blocks: VRAM first, system memory
    /// past 97% of the budget. Under memory pressure, ranges no draw used for seconds are
    /// retired: their page-table entries are cleared at the end of the submission, and once it
    /// completes their memory is unbound and reused. Bound again before that, a range takes back
    /// the memory it still holds (new memory would change under the running submission); after
    /// it, new memory and the guest data again. A shader reading a retired range through the
    /// page table faults, and the fault binds it. Unbinds and binds share each submission's one
    /// batch, which waits for nothing.
    static constexpr u64 ARENA_CHUNK_SIZE = 32_MB;
    static constexpr u32 MAX_CHUNK_WORDS = (ARENA_CHUNK_SIZE / MIN_BLOCK_SIZE) / 64;
    struct ArenaChunk {
        VmaAllocation allocation{};
        vk::DeviceMemory memory{};
        u64 offset{}; ///< of the chunk inside `memory` (a VMA block)
        bool system{};
        u32 free_blocks{};
        bool unbind_pending{}; ///< holds freed pieces whose unbind is not submitted yet
        u64 unbind_tick{};     ///< memory_semaphore tick of the last unbind from it
        std::array<u64, MAX_CHUNK_WORDS> used{};
    };
    std::vector<ArenaChunk> arena_chunks;
    u32 chunk_blocks{};
    size_t chunk_hint{};
    struct ArenaPiece {
        vk::DeviceMemory memory;
        u64 offset;
        u64 blocks;
    };
    /// Up to `blocks` contiguous blocks of backing, VRAM first. Asserts only when VRAM, system
    /// memory and an emergency reclaim all fail.
    ArenaPiece AllocateArenaPiece(u64 blocks);
    std::optional<ArenaPiece> TakeFreeBlocks(u64 blocks, bool system);
    bool AllocateArenaChunk(bool system);
    void AdoptArenaChunk(VmaAllocation allocation, vk::DeviceMemory memory, u64 offset,
                         bool system);
    void FreeArenaPiece(vk::DeviceMemory memory, u64 offset, u64 blocks);
    void ReleaseEmptyChunks(bool pressured);
    bool PrefersSystemMemory() const;
    bool IsSystemBacking(vk::DeviceMemory memory, u64 offset) const;

    /// A range retired at `tick` (the submission that last may use it), released once that
    /// submission completes.
    struct RetiredRange {
        const Buffer* arena;
        u64 tick;
        Backing backing;
    };
    std::vector<RetiredRange> retired_ranges;
    std::vector<ArenaBinds> pending_unbinds; ///< submitted in the batch with pending_binds
    u64 last_memory_signal{};
    void RetireRange(const Backing& backing, u64 tick);
    void ReleaseRetiredRanges();
    /// Drops pending unbinds of [start, end) blocks of `arena`: about to be bound again.
    void TrimPendingUnbinds(const Buffer* arena, u64 start, u64 end);
    /// Retires up to `max_bytes` of resident ranges that `pick(unit_last_use)` accepts, those
    /// backed by `system` memory if given. Returns the bytes retired.
    template <typename Pick>
    u64 RetireRanges(u64 max_bytes, std::optional<bool> system, u64 tick, Pick&& pick);
    bool EmergencyReclaim();

    /// Last use per 1 MiB of guest address space: the display frame (BbStats::gpu_frames) of
    /// its last binding, or-ed with FAULTED_UNIT once a shader read it through the page table.
    static constexpr u32 UNIT_SHIFT = 20;
    static constexpr u32 FAULTED_UNIT = 1u << 31;
    std::vector<u32> unit_last_use;
    void NoteUse(VAddr address, u64 size);
    u64 last_reclaim_frame{};
    u64 last_migrate_frame{};
    ArenaReclaimStats reclaim_stats;

    /// Allocates the next VRAM chunk on a worker thread before the current ones fill up: a new
    /// VMA block is a 32-256 MiB driver allocation the OS clears first, which hitched the
    /// recording thread when the game streamed in a new area.
    struct ChunkPrefetch {
        std::mutex mutex;
        std::condition_variable cv;
        bool requested = false, stop = false;
        VmaAllocation allocation{}; ///< a ready chunk, adopted by the recording thread
        vk::DeviceMemory memory{};
        u64 offset{};
    };
    ChunkPrefetch prefetch;
    std::thread prefetch_thread;
    void PrefetchLoop();
    bool AdoptPrefetchedChunk(bool wait);

    std::atomic<u64> arena_resident_bytes{};
    std::atomic<u64> arena_chunk_bytes{}, arena_system_bytes{};
    std::atomic<u32> arena_chunk_count{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
