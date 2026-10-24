// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "bbport_copy.h"

#include <atomic>
#include <chrono>
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
    u32 GetArenaMemoryType() const noexcept {
        return arena_memory_type_index;
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
        u64 demoted_bytes = 0;  ///< cold VRAM ranges moved to system memory (DemoteRanges)
        u64 promoted_bytes = 0; ///< hot system-memory ranges moved back to VRAM (PromoteRanges)
        u64 migrated_bytes = 0;
        u32 released_chunks = 0;
        u32 emergencies = 0;
        /// At the call: ranges bound within the last second that sit in system memory, and how
        /// much of that the GPU has written (PickRanges leaves those where they are).
        u64 hot_system_bytes = 0;
        u64 hot_system_gpu_bytes = 0;
    };
    ArenaReclaimStats TakeArenaReclaimStats();

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

    /// Invalidates the range before the guest unmaps it.
    void UnmapMemory(VAddr device_addr, u64 size);

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

    /// bbport: binds (and zeroes) the page table pages holding entries [first, end).
    void MakePageTableResident(u64 first, u64 end);

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    /// bbport: the arena for blocks [first_block, last_block]: their pages' owner, or a bridge
    /// when the pages have different owners.
    const Buffer* GetArena(u64 first_block, u64 last_block);
    /// The arena owning `page`, created over its aligned pair of pages when two pages fit the
    /// driver's buffer limit. It holds every resident block of its pages, and the page table
    /// points into it.
    const Buffer* OwnerArena(u64 page);
    const Buffer* BridgeArena(u64 first_block, u64 last_block);

    void EnsureResident(u64 first_block, u64 last_block);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// bbport: batched small copies on the copy threads (BbCopy::QueueCopy).
    static void RunGuestCopy(const BbCopy::Item& item);
    void SmallGuestCopy(const BbCopy::Item& item);

    void DescribeStaleUpload(VAddr device_addr, u32 size, bool is_written, bool is_texel_buffer);

    /// bbport: readbacks Off. A guest write into a 4 KiB page that also holds GPU-written bytes
    /// keeps a copy of the page as it was before the write (a twin); the page's upload then
    /// sends the bytes the CPU changed and leaves the GPU's own bytes alone (they were uploaded
    /// whole, stale guest bytes over live GPU data: exposure values, counters, simulation state).
    void TwinGpuPages(VAddr address, u64 size);
    /// Uploads [address, address + size) through `upload`, merging twinned pages; returns the
    /// pages that still hold GPU-written bytes afterwards.
    template <typename Upload>
    void UploadWithTwins(VAddr address, u64 size, Upload&& upload,
                         boost::container::small_vector<VAddr, 4>& kept_pages);
    void DropTwins(VAddr address, u64 size);

    const Buffer* UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                               size_t total_size_bytes);

    /// Copies the image starting at device_addr, if its GPU-written contents are the newest, into
    /// the buffer (texel reads; `for_write`: GPU writes, up to their size, metadata excluded).
    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool for_write = false);

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

    // bbport: twins of guest pages (TwinGpuPages), a fixed pool: taken in the write fault
    // handler, so it never allocates; pages beyond it upload whole as before.
    static constexpr u64 TwinPageSize = 4096;
    static constexpr size_t MaxTwins = 1024;
    struct Twin {
        VAddr page = 0; ///< 0: free
        std::array<u8, TwinPageSize> bytes;
    };
    std::mutex twin_mutex;
    std::unique_ptr<std::array<Twin, MaxTwins>> twins;
    size_t twin_high_water = 0;          ///< slots in use are below it
    std::atomic<u32> twin_count{0};
    /// bbport: GPU writes (written bindings) of the current and the previous window. A guest
    /// write into GPU-written bytes the GPU still writes takes a twin; into older ones, the
    /// GPU's bytes are downloaded first, as with readbacks Relaxed (InvalidateMemory).
    static constexpr auto GpuWriteWindow = std::chrono::milliseconds(500);
    std::mutex gpu_writes_mutex; ///< the write fault handler checks them on a guest thread
    std::array<RangeSet, 2> gpu_writes;
    u32 gpu_writes_current = 0;
    std::chrono::steady_clock::time_point gpu_writes_since{};
    std::atomic<u32> stale_downloads{0}; ///< guest writes into older GPU bytes (downloaded)
    void NoteGpuWrite(VAddr address, u64 size);
    bool GpuWrittenRecently(VAddr address, u64 size);
    void RotateGpuWrites(std::chrono::steady_clock::time_point now);

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;
    /// bbport: the page table is sparse: one bit per table page (block_size bytes of entries)
    /// bound to memory, and that memory. Pages are bound on their first entry write.
    std::vector<u64> pagetable_bound;
    std::vector<VmaAllocation> pagetable_memory;

    std::array<const Buffer*, MAX_ARENA_PAGES> address_space{};
    u64 arena_page_bits{MAX_ARENA_PAGE_BITS};
    u64 arena_page_size{u64{1} << MAX_ARENA_PAGE_BITS};
    u64 num_arena_pages{u64{1} << (ADDRESS_SPACE_BITS - MAX_ARENA_PAGE_BITS)};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    /// Blocks [start, end) bound to `memory` from byte `offset`.
    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        /// bbport: start and end count blocks, the offset bytes. Splitting a backing (retiring
        /// part of it, Subtract) added blocks to bytes: the parts pointed a few bytes past the
        /// original offset instead of N blocks, so releasing one freed the blocks of another
        /// still in use (two ranges on one memory: wrong data; then "Empty buffer arena piece"
        /// once the free counts and bitmaps disagreed).
        u32 block_shift;
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory &&
                   offset + ((end - start) << block_shift) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + ((a - start) << block_shift), block_shift};
        }
    };
    IntervalList<Backing> resident_ranges;

    /// bbport: arenas stay within the driver's buffer limit (2 GiB on an RX 5500M, a pair of
    /// 1 GiB pages). A buffer across two owners comes from a bridge: an arena over both sides
    /// of their boundary, mirroring the bindings of a window around it that grows with the
    /// requests. Bloodborne's GPU memory crosses two boundaries between pairs (0x1080000000,
    /// 0x1100000000). Before, such a buffer replaced both sides' arenas by one over both, of up
    /// to 4 GiB, binding every resident range again (1401-1893 binds, then a ~600 ms frame);
    /// smeared or heavily blurred textures followed in three RX 5500M runs that did so.
    struct Bridge {
        const Buffer* arena;
        u64 lo, hi; ///< blocks [lo, hi) mirrored: those resident are bound in it too
    };
    std::vector<Bridge> bridges;
    bool arena_pairs{};
    /// Calls func(arena, start, end) for each arena binding of blocks [start, end): the owners'
    /// (one call per run of pages with the same owner) and those of bridges covering them.
    template <typename Func>
    void ForEachArenaBinding(u64 start, u64 end, Func&& func) const;
    /// Queues the binds of `backing` into the arenas mapping its blocks (bridges only when
    /// `owners` is false: the owners still hold it).
    void BindBacking(const Backing& backing, bool owners);
    /// Binds the resident blocks of [start, end) into `bridge` (its window grows over them).
    void MirrorIntoBridge(const Bridge& bridge, u64 start, u64 end);

    u32 arena_memory_type_index{};
    /// bbport: system-memory type for arena blocks when VRAM runs out, ~0u if none fits.
    u32 arena_fallback_memory_type_index = ~0u;

    /// bbport: arena blocks come from 8 MiB VMA chunks (sharing VMA's blocks, and the space
    /// evicted images leave there), each with a bitmap of used blocks: VRAM first, system memory
    /// past 97% of the budget. 8 MiB fits the holes evictions leave in VMA blocks: with 32 MiB
    /// chunks, an RX 5500M put 288 MiB of vertex data in system memory while 80 MiB sat free in
    /// VRAM in smaller pieces. Under memory pressure, ranges no draw used for seconds are
    /// retired: their page-table entries are cleared at the end of the submission, and once it
    /// completes their memory is unbound and reused. Bound again before that, a range takes back
    /// the memory it still holds (new memory would change under the running submission); after
    /// it, new memory and the guest data again. A shader reading a retired range through the
    /// page table faults, and the fault binds it. Unbinds and binds share each submission's one
    /// batch, which waits for nothing.
    static constexpr u64 ARENA_CHUNK_SIZE = 8_MB;
    static constexpr u32 MAX_CHUNK_WORDS = (ARENA_CHUNK_SIZE / MIN_BLOCK_SIZE) / 64;
    struct ArenaChunk {
        VmaAllocation allocation{};
        vk::DeviceMemory memory{};
        u64 offset{}; ///< of the chunk inside `memory` (a VMA block)
        bool system{};
        u32 free_blocks{};
        bool unbind_pending{}; ///< holds freed pieces whose unbind is not submitted yet
        u64 unbind_tick{};     ///< memory_semaphore tick of the last unbind from it
        u8* mapped{};          ///< system chunks: the chunk in host memory (VMA keeps it mapped)
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
    bool AllocateArenaChunk(bool system, bool existing_blocks = false);
    void AdoptArenaChunk(VmaAllocation allocation, vk::DeviceMemory memory, u64 offset,
                         bool system, void* mapped = nullptr);
    void FreeArenaPiece(vk::DeviceMemory memory, u64 offset, u64 blocks);
    /// `keep_vram`: no empty VRAM chunk goes back (arena data in system memory will need them).
    void ReleaseEmptyChunks(bool pressured, bool keep_vram);
    bool PrefersSystemMemory() const;
    /// Enough space free inside VRAM's VMA blocks that an arena chunk may take some.
    bool HasSpareVmaSpace() const;
    bool IsSystemBacking(vk::DeviceMemory memory, u64 offset) const;

    /// A range retired at `tick` (the submission that last may use it), released once that
    /// submission completes.
    struct RetiredRange {
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

    /// bbport: resident ranges (backed by `system` memory if given) whose `pick(unit_last_use)`
    /// accepts them and the GPU has not modified, up to `max_bytes`, in address order (merged
    /// where contiguous).
    template <typename Pick>
    boost::container::small_vector<Backing, 64> PickRanges(u64 max_bytes,
                                                           std::optional<bool> system,
                                                           Pick&& pick) const;
    /// bbport: moves picked ranges from VRAM to system memory at the same arena addresses, so
    /// the page table stays valid throughout (unlike retiring them): their guest data is copied
    /// into mapped system blocks by the CPU, the arenas are bound to those in the batch sent with
    /// submission `tick`, and the VRAM blocks are freed once that submission completes. Returns
    /// the bytes moved.
    template <typename Pick>
    u64 DemoteRanges(u64 max_bytes, u64 tick, Pick&& pick);
    /// The backing a demoted or promoted range had, freed once `tick` completes (submissions
    /// before it may still read it).
    struct ReplacedRange {
        u64 tick;
        Backing backing;
    };
    std::vector<ReplacedRange> replaced_ranges;
    void ReleaseReplacedRanges();
    /// Host pointer of system arena memory at (memory, offset), or nullptr.
    u8* SystemArenaPointer(vk::DeviceMemory memory, u64 offset) const;

    /// bbport: hot ranges in system memory go back to free VRAM blocks: the GPU copies them in
    /// submission `copy_tick` into `promote_scratch`, where the VRAM piece is bound (sparse
    /// aliasing); once that completes, the arenas are bound to the piece (FinishPromotions).
    /// Both copies hold the same data at the switch, so submissions still running read either.
    struct Promotion {
        u64 copy_tick;
        Backing from;       ///< the system backing of blocks [from.start, from.end)
        ArenaPiece to;      ///< VRAM blocks, as many as `from`
        u64 scratch_offset; ///< where `to` is bound in promote_scratch
        bool cancelled;     ///< arena data written since the copy (uploads, image syncs)
    };
    static constexpr u64 PROMOTE_SCRATCH_SIZE = u64{64} << 20;
    std::vector<Promotion> promotions;
    const Buffer* promote_scratch{};
    template <typename Pick>
    u64 PromoteRanges(u64 max_bytes, u64 tick, Pick&& pick);
    /// Switches the arenas to the VRAM copies whose copy completed; returns whether any ended.
    bool FinishPromotions();
    /// Arena data of guest [address, address + size) is being written: promotions copied before
    /// would switch to stale data.
    void CancelPromotions(VAddr address, u64 size);
    bool hot_in_system{}; ///< hot ranges in system memory at the last arena pass

    /// Last use per 1 MiB of guest address space: the display frame (BbStats::gpu_frames) of
    /// its last binding, or-ed with FAULTED_UNIT once a shader read it through the page table.
    static constexpr u32 UNIT_SHIFT = 20;
    static constexpr u32 FAULTED_UNIT = 1u << 31;
    std::vector<u32> unit_last_use;
    void NoteUse(VAddr address, u64 size);
    u64 last_reclaim_frame{};
    u64 last_demote_frame{};
    u64 last_promote_frame{};
    u64 last_migrate_frame{};
    ArenaReclaimStats reclaim_stats;

    /// Allocates the next VRAM chunk on a worker thread before the current ones fill up: a new
    /// VMA block is a driver allocation the OS clears first, which hitched the recording thread
    /// when the game streamed in a new area.
    struct ChunkPrefetch {
        std::mutex mutex;
        std::condition_variable cv;
        bool requested = false, stop = false;
        VmaAllocation allocation{}; ///< a ready chunk, adopted by the recording thread
        vk::DeviceMemory memory{};
        u64 offset{};
        /// bbport: the same for a system-memory chunk (demotion's destination).
        bool system_requested = false;
        VmaAllocation system_allocation{};
        vk::DeviceMemory system_memory{};
        u64 system_offset{};
        void* system_mapped{};
    };
    ChunkPrefetch prefetch;
    std::thread prefetch_thread;
    void PrefetchLoop();
    bool AdoptPrefetchedChunk(bool wait);
    /// bbport: adopts the worker's system-memory chunk; without one, asks for it and fails.
    bool AdoptPrefetchedSystemChunk();

    std::atomic<u64> arena_resident_bytes{};
    std::atomic<u64> arena_chunk_bytes{}, arena_system_bytes{};
    std::atomic<u32> arena_chunk_count{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
