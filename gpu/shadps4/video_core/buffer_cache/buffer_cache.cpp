// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <array>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <magic_enum/magic_enum.hpp>
#include "bbport_copy.h"
#include "bbport_toggles.h"
#include "bbport_watchdog.h"
#include "bbport_hitch.h"
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    while (arena_page_bits > MIN_ARENA_PAGE_BITS &&
           (u64{2} << arena_page_bits) > instance.GetMaxBufferSize()) {
        --arena_page_bits;
    }
    arena_page_size = u64{1} << arena_page_bits;
    num_arena_pages = u64{1} << (ADDRESS_SPACE_BITS - arena_page_bits);
    arena_pairs = (u64{2} << arena_page_bits) <= instance.GetMaxBufferSize();
    std::printf("Buffer cache: arena pages of %llu MiB, %s (driver buffer limit %llu MiB)\n",
                (unsigned long long)(arena_page_size >> 20),
                arena_pairs ? "owned in pairs" : "owned singly",
                (unsigned long long)(instance.GetMaxBufferSize() >> 20));
    const vk::BufferCreateInfo probe_ci = {
        .flags = vk::BufferCreateFlagBits::eSparseBinding |
                 vk::BufferCreateFlagBits::eSparseResidency |
                 (instance.IsSparseAliasingSupported() ? vk::BufferCreateFlagBits::eSparseAliased
                                                       : vk::BufferCreateFlags{}),
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();
    const auto& memory_props = instance.GetMemoryProperties();
    u32 system_type_bits = 0;
    for (u32 i = 0; i < memory_props.memoryTypeCount; ++i) {
        if (!(memory_props.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            system_type_bits |= 1u << i;
        }
    }
    arena_fallback_memory_type_index =
        FindMemoryType(memory_props, vk::MemoryPropertyFlagBits::eHostVisible,
                       reqs.memoryTypeBits & system_type_bits)
            .value_or(~0u);
    // bbport: which memory the sparse arenas may use (OOM investigation, 4 GB GPUs).
    std::printf("Buffer cache: arena blocks of %u KiB in %llu MiB chunks, device allocations "
                "allowed %u, memory types 0x%x, using type %u",
                block_size >> 10, (unsigned long long)(ARENA_CHUNK_SIZE >> 20),
                instance.GetMaxMemoryAllocationCount(), reqs.memoryTypeBits,
                arena_memory_type_index);
    if (arena_fallback_memory_type_index != ~0u) {
        std::printf(", system-memory fallback type %u\n", arena_fallback_memory_type_index);
    } else {
        std::printf(", no system-memory fallback\n");
    }
    for (u32 i = 0; i < memory_props.memoryTypeCount; ++i) {
        const auto& type = memory_props.memoryTypes[i];
        std::printf("  memory type %u: heap %u (%llu MiB), %s%s\n", i, type.heapIndex,
                    (unsigned long long)(memory_props.memoryHeaps[type.heapIndex].size >> 20),
                    vk::to_string(type.propertyFlags).c_str(),
                    ((reqs.memoryTypeBits >> i) & 1) ? ", allowed for arenas" : "");
    }

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * num_arena_pages) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * num_arena_pages);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);

    chunk_blocks = static_cast<u32>(ARENA_CHUNK_SIZE >> block_shift);
    ASSERT_MSG(chunk_blocks <= MAX_CHUNK_WORDS * 64, "Arena chunk of {} blocks", chunk_blocks);
    unit_last_use.assign(u64{1} << (ADDRESS_SPACE_BITS - UNIT_SHIFT), 0);
    prefetch_thread = std::thread([this] { PrefetchLoop(); });
}

BufferCache::~BufferCache() {
    {
        std::scoped_lock lock{prefetch.mutex};
        prefetch.stop = true;
        prefetch.cv.notify_all();
    }
    prefetch_thread.join();
    if (prefetch.allocation) {
        vmaFreeMemory(instance.GetAllocator(), prefetch.allocation);
    }
    for (const ArenaChunk& chunk : arena_chunks) {
        vmaFreeMemory(instance.GetAllocator(), chunk.allocation);
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

// bbport: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in Hunter's Nightmare, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const BbCopy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
void BufferCache::SmallGuestCopy(const BbCopy::Item& item) {
    if (scheduler.IsRecordingDeferred() && !BbToggle::Disabled(BbToggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
        return;
    }
    BbCopy::QueueCopy(item);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static const u64 window = [] {
        const char* env = std::getenv("BB_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= TRACKER_BYTES_PER_PAGE || BbToggle::Disabled(BbToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    const auto flush_request = [this, device_addr, size, is_write] {
        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr window_start = Common::AlignDown(device_addr, WindowSize);
        const VAddr window_end = std::max<VAddr>(window_start + WindowSize, device_addr + size);
        // bbport: from the owner arenas, which hold every resident block (GPU-written ones
        // included); a page without one has nothing to download.
        for (VAddr start = window_start; start < window_end;) {
            const u64 page = start >> arena_page_bits;
            const VAddr end = std::min<VAddr>((page + 1) << arena_page_bits, window_end);
            if (const Buffer* owner = address_space[page]) {
                DownloadMemory(owner, start, end - start);
            }
            start = end;
        }
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    scheduler.Finish();

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

namespace {
// bbport: BB_BUFFER_STATS=1 — how buffer bindings reach the GPU, by guest region (256 MiB),
// printed every 5 s: small read-only copies into the stream buffer, arena bindings, and the
// bytes those re-upload after CPU writes. Input for engine-level short paths.
struct BufferStats {
    struct Region {
        u64 stream_count{}, stream_bytes{}, arena_count{}, arena_bytes{}, upload_bytes{};
    };
    std::map<u64, Region> regions;
    /// Hot window: frame of the last binding per 64 KiB block, and the distribution of the
    /// frames between uses (1, 2, 3, 4, 5-8, 9-16, 17+): the ring's reuse distance.
    std::unordered_map<u64, u64> last_use;
    std::array<u64, 7> reuse{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 calls = 0;
};
/// 256 MiB regions; 1 MiB inside the hot 0x104xxxxxxx window (the engine's frame data).
BufferStats& Stats();
void NoteHotUse(VAddr address, u32 size) {
    if ((address >> 28) != 0x104) {
        return;
    }
    auto& st = Stats();
    const u64 frame = BbStats::gpu_frames.load(std::memory_order_relaxed);
    for (u64 block = address >> 16; block <= (address + size - 1) >> 16; ++block) {
        auto [it, inserted] = st.last_use.try_emplace(block, frame);
        if (!inserted && it->second != frame) {
            const u64 d = frame - it->second;
            ++st.reuse[d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : 6];
            it->second = frame;
        }
    }
}
u64 RegionKey(VAddr address) {
    return (address >> 28) == 0x104 ? (address >> 20) | (1ull << 40) : address >> 28;
}
bool BufferStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
BufferStats& Stats() {
    static BufferStats stats;
    return stats;
}
void PrintBufferStats() {
    auto& st = Stats();
    if ((++st.calls & 4095) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds < 5.0) {
        return;
    }
    std::printf("Buffer stats (%.1f s, per second): region, stream copies/bytes, arena bindings, "
                "re-uploaded bytes\n", seconds);
    std::printf("  hot window reuse distance in frames (1,2,3,4,5-8,9-16,17+): %llu %llu %llu %llu "
                "%llu %llu %llu\n",
                (unsigned long long)st.reuse[0], (unsigned long long)st.reuse[1],
                (unsigned long long)st.reuse[2], (unsigned long long)st.reuse[3],
                (unsigned long long)st.reuse[4], (unsigned long long)st.reuse[5],
                (unsigned long long)st.reuse[6]);
    st.reuse = {};
    for (const auto& [region, r] : st.regions) {
        const u64 base = (region >> 40) ? (region & ((1ull << 40) - 1)) << 20 : region << 28;
        if ((region >> 40) && r.stream_bytes + r.upload_bytes < 1e6 * seconds) {
            continue; // quiet MiB of the hot window
        }
        std::printf("  %#012llx: %8.0f copies %8.2f MB, %8.0f arena %8.2f MB bound, %8.2f MB uploads\n",
                    static_cast<unsigned long long>(base), r.stream_count / seconds,
                    r.stream_bytes / seconds / 1e6, r.arena_count / seconds,
                    r.arena_bytes / seconds / 1e6,
                    r.upload_bytes / seconds / 1e6);
    }
    st.regions.clear();
    st.window = now;
}
} // namespace

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    const bool stats = BufferStatsEnabled();
    if (stats) {
        PrintBufferStats();
        NoteHotUse(device_addr, size);
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        if (stats) {
            auto& region = Stats().regions[RegionKey(device_addr)];
            ++region.stream_count;
            region.stream_bytes += size;
        }
        // bbport: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !BbToggle::Disabled(BbToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    NoteUse(device_addr, size);
    EnsureResident(first_block, last_block);
    const u64 uploaded_before = BbStats::buffer_upload_bytes.load(std::memory_order_relaxed);
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (stats) {
        auto& region = Stats().regions[RegionKey(device_addr)];
        ++region.arena_count;
        region.arena_bytes += size;
        region.upload_bytes +=
            BbStats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before;
    }
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        // bbport: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const BbCopy::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                BbCopy::Async([item] { item.run(item); });
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    for (const auto& range : resident_ranges) {
        // bbport: through the owners, per run of pages (a range may cross into the next pair).
        ForEachArenaBinding(range.start, range.end, [&](const Buffer* arena, u64 start, u64 end) {
            if (address_space[start >> blocks_per_arena_page_shift] == arena) {
                SynchronizeMemory(arena, start << block_shift, (end - start) << block_shift,
                                  false, false);
            }
        });
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const Buffer* first_arena = OwnerArena(first_page);
    if (OwnerArena(last_page) == first_arena) {
        return first_arena;
    }
    return BridgeArena(first_block, last_block);
}

const Buffer* BufferCache::OwnerArena(u64 page) {
    if (const Buffer* arena = address_space[page]) {
        return arena;
    }
    // Owners are created whole, so the other page of the pair has none either.
    const u64 first = arena_pairs ? page & ~u64{1} : page;
    const u64 count = arena_pairs && first + 1 < num_arena_pages ? 2 : 1;
    const Buffer* arena = &arenas.emplace_back(instance, first << arena_page_bits,
                                               count << arena_page_bits, MemoryType::Sparse);
    for (u64 owned = first; owned < first + count; ++owned) {
        address_space[owned] = arena;
    }
    return arena;
}

const Buffer* BufferCache::BridgeArena(u64 first_block, u64 last_block) {
    const u64 end_block = last_block + 1;
    auto it = std::ranges::find_if(bridges, [&](const Bridge& bridge) {
        const u64 base = bridge.arena->cpu_addr >> block_shift;
        return base <= first_block &&
               end_block <= base + (bridge.arena->size_bytes >> block_shift);
    });
    if (it == bridges.end()) {
        // Centred on the boundary between the two owners (the pages on both sides), or moved
        // over the request when the driver's limit makes it narrower than two pages.
        const u64 max_blocks = std::min<u64>(u64{2} << blocks_per_arena_page_shift,
                                             instance.GetMaxBufferSize() >> block_shift);
        ASSERT_MSG(end_block - first_block <= max_blocks,
                   "A {} MiB buffer is over the driver's buffer size limit",
                   ((end_block - first_block) << block_shift) >> 20);
        const u64 boundary = (last_block >> blocks_per_arena_page_shift)
                             << blocks_per_arena_page_shift;
        const u64 lowest = end_block > max_blocks ? end_block - max_blocks : 0;
        const u64 base = std::clamp(boundary - max_blocks / 2, lowest, first_block);
        const Buffer* arena = &arenas.emplace_back(instance, base << block_shift,
                                                   max_blocks << block_shift, MemoryType::Sparse);
        it = bridges.insert(bridges.end(), Bridge{arena, 0, 0});
        std::printf("Buffer cache: bridge arena over 0x%llx-0x%llx for buffers across 0x%llx\n",
                    (unsigned long long)(base << block_shift),
                    (unsigned long long)((base + max_blocks) << block_shift),
                    (unsigned long long)(boundary << block_shift));
    }
    // The window grows over the request, in 8 MiB steps: binding the resident ranges it gains
    // costs a few binds, where replacing the arenas bound everything.
    Bridge& bridge = *it;
    const u64 base = bridge.arena->cpu_addr >> block_shift;
    const u64 limit = base + (bridge.arena->size_bytes >> block_shift);
    const u64 step = std::max<u64>(8_MB >> block_shift, 1);
    const u64 lo = std::max(base, Common::AlignDown(first_block, step));
    const u64 hi = std::min(limit, Common::AlignUp(end_block, step));
    if (bridge.lo == bridge.hi) {
        MirrorIntoBridge(bridge, lo, hi);
        bridge.lo = lo;
        bridge.hi = hi;
    }
    if (lo < bridge.lo) {
        MirrorIntoBridge(bridge, lo, bridge.lo);
        bridge.lo = lo;
    }
    if (bridge.hi < hi) {
        MirrorIntoBridge(bridge, bridge.hi, hi);
        bridge.hi = hi;
    }
    return bridge.arena;
}

template <typename Func>
void BufferCache::ForEachArenaBinding(u64 start, u64 end, Func&& func) const {
    for (u64 block = start; block < end;) {
        const Buffer* owner = address_space[block >> blocks_per_arena_page_shift];
        u64 run_end = block;
        do {
            run_end = std::min<u64>(
                end, ((run_end >> blocks_per_arena_page_shift) + 1) << blocks_per_arena_page_shift);
        } while (run_end < end && address_space[run_end >> blocks_per_arena_page_shift] == owner);
        if (owner) {
            func(owner, block, run_end);
        }
        block = run_end;
    }
    for (const Bridge& bridge : bridges) {
        const u64 a = std::max(start, bridge.lo);
        const u64 b = std::min(end, bridge.hi);
        if (a < b) {
            func(bridge.arena, a, b);
        }
    }
}

void BufferCache::BindBacking(const Backing& backing, bool owners) {
    ForEachArenaBinding(backing.start, backing.end, [&](const Buffer* arena, u64 start, u64 end) {
        if (!owners && address_space[start >> blocks_per_arena_page_shift] == arena) {
            return;
        }
        BindsForArena(arena)->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start << block_shift) - arena->cpu_addr,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = backing.offset + ((start - backing.start) << block_shift),
        });
    });
}

void BufferCache::MirrorIntoBridge(const Bridge& bridge, u64 start, u64 end) {
    auto* bind = BindsForArena(bridge.arena);
    resident_ranges.ForEachInRange(start, end, [&](const Backing& backing) {
        const u64 a = std::max(start, backing.start);
        const u64 b = std::min(end, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (a << block_shift) - bridge.arena->cpu_addr,
            .size = (b - a) << block_shift,
            .memory = backing.memory,
            .memoryOffset = backing.offset + ((a - backing.start) << block_shift),
        });
    });
}

void BufferCache::EnsureResident(u64 first_block, u64 last_block) {
    // bbport: a range retired but not released yet is still bound to its memory (the
    // submission that retired it may still run): it takes that memory back. Binding new memory
    // would change what the running submission reads.
    boost::container::small_vector<Backing, 4> restored;
    for (size_t i = 0; i < retired_ranges.size();) {
        const RetiredRange& retired = retired_ranges[i];
        if (retired.backing.start <= last_block && first_block < retired.backing.end) {
            restored.push_back(retired.backing);
            resident_ranges.Add(retired.backing);
            retired_ranges.erase(retired_ranges.begin() + i);
        } else {
            ++i;
        }
    }

    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });
    for (const Backing& backing : restored) {
        resident_blocks += backing.end - backing.start;
    }

    if (bind_ranges.Empty() && restored.empty()) {
        return;
    }
    BbStats::Timer timer{BbStats::t_resident};

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    // Page-table entries for blocks [start, end), now backed: into the owners, which hold every
    // resident block (a bridge only those near its boundary).
    const auto write_entries = [&](u64 start, u64 end) {
        for (u64 block = start; block < end; ++block) {
            const Buffer* owner = address_space[block >> blocks_per_arena_page_shift];
            *(bda_addrs++) =
                owner->BufferDeviceAddress() + ((block << block_shift) - owner->cpu_addr);
        }
        const u64 copy_size = (end - start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
        arena_resident_bytes.fetch_add((end - start) << block_shift, std::memory_order_relaxed);
    };
    for (const Backing& backing : restored) {
        // A bridge whose window grew over it while it was retired lacks its binding.
        BindBacking(backing, false);
        write_entries(backing.start, backing.end);
    }
    for (const auto& range : bind_ranges) {
        // Released earlier in this submission: its unbinds must not follow these binds.
        ForEachArenaBinding(range.start, range.end,
                            [this](const Buffer* arena, u64 start, u64 end) {
                                TrimPendingUnbinds(arena, start, end);
                            });
        // Free blocks come in runs: bind the range piece by piece.
        for (u64 start = range.start; start < range.end;) {
            const ArenaPiece piece = AllocateArenaPiece(range.end - start);
            // A bitmap out of step with its free count would loop here forever.
            ASSERT_MSG(piece.blocks != 0, "Empty buffer arena piece");
            Backing backing;
            backing.start = start;
            backing.end = start + piece.blocks;
            backing.memory = piece.memory;
            backing.offset = piece.offset;
            resident_ranges.Add(backing);

            LOG_INFO(Render, "Making range start={}, end={} resident", backing.start,
                     backing.end);

            BindBacking(backing, true);
            write_entries(backing.start, backing.end);
            start = backing.end;
        }
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

void BufferCache::TrimPendingUnbinds(const Buffer* arena, u64 start, u64 end) {
    const auto it = std::ranges::find(pending_unbinds, arena, &ArenaBinds::arena);
    if (it == pending_unbinds.end()) {
        return;
    }
    const u64 lo = (start << block_shift) - arena->cpu_addr;
    const u64 hi = (end << block_shift) - arena->cpu_addr;
    auto& binds = it->binds;
    for (size_t i = 0; i < binds.size();) {
        const u64 a = binds[i].resourceOffset;
        const u64 b = a + binds[i].size;
        if (b <= lo || hi <= a) {
            ++i;
            continue;
        }
        // Keep the parts outside [lo, hi).
        if (hi < b) {
            binds.push_back(vk::SparseMemoryBind{.resourceOffset = hi, .size = b - hi});
        }
        if (a < lo) {
            binds[i].size = lo - a;
            ++i;
        } else {
            binds.erase(binds.begin() + i);
        }
    }
}

namespace {
/// bbport: one arena chunk of `type` from VMA (worker thread or recording thread), only from
/// space VMA already holds when `existing_blocks`.
bool AllocateChunkMemory(VmaAllocator allocator, u32 type, u64 size, u64 alignment,
                         VmaAllocation& allocation, VmaAllocationInfo& info,
                         bool existing_blocks = false) {
    const VkMemoryRequirements requirements = {
        .size = size,
        .alignment = alignment,
        .memoryTypeBits = 1u << type,
    };
    const VmaAllocationCreateInfo create_info = {
        .flags = existing_blocks ? VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT : 0u,
        .memoryTypeBits = 1u << type,
    };
    return vmaAllocateMemory(allocator, &requirements, &create_info, &allocation, &info) ==
           VK_SUCCESS;
}

void SetBits(std::span<u64> words, u32 first, u32 count, bool value) {
    for (u32 bit = first; bit < first + count; ++bit) {
        const u64 mask = u64{1} << (bit & 63);
        words[bit >> 6] = value ? words[bit >> 6] | mask : words[bit >> 6] & ~mask;
    }
}
} // namespace

void BufferCache::NoteUse(VAddr address, u64 size) {
    const u32 frame =
        static_cast<u32>(BbStats::gpu_frames.load(std::memory_order_relaxed)) & ~FAULTED_UNIT;
    const u64 last = std::min<u64>((address + std::max<u64>(size, 1) - 1) >> UNIT_SHIFT,
                                   unit_last_use.size() - 1);
    for (u64 unit = address >> UNIT_SHIFT; unit <= last; ++unit) {
        unit_last_use[unit] = (unit_last_use[unit] & FAULTED_UNIT) | frame;
    }
}

void BufferCache::NoteFaultedRange(VAddr address, u64 size) {
    NoteUse(address, size);
    const u64 last = std::min<u64>((address + std::max<u64>(size, 1) - 1) >> UNIT_SHIFT,
                                   unit_last_use.size() - 1);
    for (u64 unit = address >> UNIT_SHIFT; unit <= last; ++unit) {
        unit_last_use[unit] |= FAULTED_UNIT;
    }
}

bool BufferCache::PrefersSystemMemory() const {
    // bbport: arenas hold vertex, index and constant data read by every draw, so they take
    // VRAM before images: a new chunk goes to system memory only past 97% of the live budget,
    // above the texture cache's aggressive mark (95% by default), which then evicts images to
    // make room. Past the budget Windows would page VRAM out instead. Checked for every chunk,
    // so arenas return to VRAM when the budget grows back.
    // The driver's own figure must also leave 96 MiB: the space free inside VMA blocks is
    // scattered, and images and staging blocks need whole new allocations (the texture cache
    // evicts from 64 MiB before the heap's budget).
    if (arena_fallback_memory_type_index == ~0u || !instance.CanReportMemoryUsage()) {
        return false;
    }
    const auto levels = instance.GetMemoryLevels();
    const u64 effective = levels.usage - std::min(levels.usage, levels.vma_free);
    return levels.budget != 0 && (effective + ARENA_CHUNK_SIZE > levels.budget / 100 * 97 ||
                                  levels.usage + ARENA_CHUNK_SIZE + 96_MB > levels.budget);
}

bool BufferCache::IsSystemBacking(vk::DeviceMemory memory, u64 offset) const {
    for (const ArenaChunk& chunk : arena_chunks) {
        if (chunk.memory == memory && offset >= chunk.offset &&
            offset < chunk.offset + ARENA_CHUNK_SIZE) {
            return chunk.system;
        }
    }
    return false;
}

std::optional<BufferCache::ArenaPiece> BufferCache::TakeFreeBlocks(u64 blocks, bool system) {
    const size_t count = arena_chunks.size();
    for (size_t i = 0; i < count; ++i) {
        const size_t index = (chunk_hint + i) % count;
        ArenaChunk& chunk = arena_chunks[index];
        if (chunk.system != system || chunk.free_blocks == 0) {
            continue;
        }
        // First free run, at most `blocks` long.
        u32 first = 0;
        for (u32 word = 0; word * 64 < chunk_blocks; ++word) {
            if (chunk.used[word] != ~u64{0}) {
                first = word * 64 + std::countr_one(chunk.used[word]);
                break;
            }
        }
        u32 length = 0;
        while (first + length < chunk_blocks && length < blocks &&
               !((chunk.used[(first + length) >> 6] >> ((first + length) & 63)) & 1)) {
            ++length;
        }
        SetBits(chunk.used, first, length, true);
        chunk.free_blocks -= length;
        chunk_hint = index;
        return ArenaPiece{chunk.memory, chunk.offset + (u64{first} << block_shift), length};
    }
    return std::nullopt;
}

BufferCache::ArenaPiece BufferCache::AllocateArenaPiece(u64 blocks) {
    if (const auto piece = TakeFreeBlocks(blocks, false)) {
        return *piece;
    }
    const BbWatch::ScopedStage stage{"allocating a buffer arena chunk"};
    const bool system_first = PrefersSystemMemory();
    if (!system_first && (AdoptPrefetchedChunk(true) || AllocateArenaChunk(false))) {
        return *TakeFreeBlocks(blocks, false);
    }
    // VRAM short: a chunk may still fit in space VMA already holds, which costs the heap nothing.
    if (system_first && AllocateArenaChunk(false, true)) {
        return *TakeFreeBlocks(blocks, false);
    }
    if (const auto piece = TakeFreeBlocks(blocks, true)) {
        return *piece;
    }
    if (arena_fallback_memory_type_index != ~0u && AllocateArenaChunk(true)) {
        return *TakeFreeBlocks(blocks, true);
    }
    // System memory refused too: VRAM past the headroom mark beats failing.
    if (system_first && AllocateArenaChunk(false)) {
        return *TakeFreeBlocks(blocks, false);
    }
    // bbport: refused at the driver's budget: idle staging, then the VRAM reserve make room.
    for (u32 attempt = 0; attempt < 4 && HandleOutOfMemory(); ++attempt) {
        if (AllocateArenaChunk(false)) {
            return *TakeFreeBlocks(blocks, false);
        }
    }
    if (EmergencyReclaim()) {
        if (const auto piece = TakeFreeBlocks(blocks, false)) {
            return *piece;
        }
        if (const auto piece = TakeFreeBlocks(blocks, true)) {
            return *piece;
        }
    }
    LOG_CRITICAL(Render_Vulkan,
                 "Failed allocating a {} MiB buffer arena chunk; {} chunks ({} MiB, {} MiB in "
                 "system memory), {} MiB resident; device usage {} of {} MiB budget; {}",
                 ARENA_CHUNK_SIZE >> 20, GetArenaChunkCount(), GetArenaChunkBytes() >> 20,
                 GetArenaSystemBytes() >> 20, GetArenaResidentBytes() >> 20,
                 instance.GetDeviceMemoryUsage() >> 20, instance.GetDeviceMemoryBudgetNow() >> 20,
                 instance.DescribeMemory());
    ASSERT_MSG(false, "Out of memory for buffer arenas");
    return {};
}

bool BufferCache::AllocateArenaChunk(bool system, bool existing_blocks) {
    const u32 type = system ? arena_fallback_memory_type_index : arena_memory_type_index;
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (!AllocateChunkMemory(instance.GetAllocator(), type, ARENA_CHUNK_SIZE, block_size,
                             allocation, info, existing_blocks)) {
        if (!existing_blocks) {
            LOG_WARNING(Render_Vulkan, "Buffer arena chunk in {} memory failed; {}",
                        system ? "system" : "video", instance.DescribeMemory());
        }
        return false;
    }
    AdoptArenaChunk(allocation, info.deviceMemory, info.offset, system);
    return true;
}

void BufferCache::AdoptArenaChunk(VmaAllocation allocation, vk::DeviceMemory memory, u64 offset,
                                  bool system) {
    ArenaChunk& chunk = arena_chunks.emplace_back();
    chunk.allocation = allocation;
    chunk.memory = memory;
    chunk.offset = offset;
    chunk.system = system;
    chunk.free_blocks = chunk_blocks;
    chunk_hint = arena_chunks.size() - 1;
    const u32 count = arena_chunk_count.fetch_add(1, std::memory_order_relaxed) + 1;
    BbHitch::arena_chunks.fetch_add(1, std::memory_order_relaxed);
    arena_chunk_bytes.fetch_add(ARENA_CHUNK_SIZE, std::memory_order_relaxed);
    if (system) {
        const u64 system_bytes =
            arena_system_bytes.fetch_add(ARENA_CHUNK_SIZE, std::memory_order_relaxed) +
            ARENA_CHUNK_SIZE;
        LOG_WARNING(Render_Vulkan,
                    "VRAM near the budget: buffer arena chunk {} in system memory ({} MiB "
                    "there); device usage {} of {} MiB budget",
                    count, system_bytes >> 20, instance.GetDeviceMemoryUsage() >> 20,
                    instance.GetDeviceMemoryBudgetNow() >> 20);
    }
}

void BufferCache::FreeArenaPiece(vk::DeviceMemory memory, u64 offset, u64 blocks) {
    // A backing merges pieces contiguous in memory, which can span adjacent chunks.
    while (blocks != 0) {
        const auto it = std::ranges::find_if(arena_chunks, [&](const ArenaChunk& chunk) {
            return chunk.memory == memory && offset >= chunk.offset &&
                   offset < chunk.offset + ARENA_CHUNK_SIZE;
        });
        ASSERT_MSG(it != arena_chunks.end(), "Freed arena memory belongs to no chunk");
        const u32 first = static_cast<u32>((offset - it->offset) >> block_shift);
        const u32 count = static_cast<u32>(std::min<u64>(blocks, chunk_blocks - first));
        SetBits(it->used, first, count, false);
        it->free_blocks += count;
        it->unbind_pending = true;
        blocks -= count;
        offset += u64{count} << block_shift;
    }
}

void BufferCache::ReleaseEmptyChunks(bool pressured) {
    // Empty system-memory chunks go back at once; one empty VRAM chunk stays as the next
    // allocation's, unless VRAM is short. A chunk goes only after the unbinds of its pieces
    // ran, so no arena still maps it when VMA reuses the memory.
    bool refreshed = false;
    bool kept_spare = pressured;
    for (size_t i = arena_chunks.size(); i-- > 0;) {
        ArenaChunk& chunk = arena_chunks[i];
        if (chunk.free_blocks != chunk_blocks || chunk.unbind_pending) {
            continue;
        }
        if (!chunk.system && !kept_spare) {
            kept_spare = true;
            continue;
        }
        if (!memory_semaphore.IsFree(chunk.unbind_tick)) {
            if (refreshed) {
                continue;
            }
            memory_semaphore.Refresh();
            refreshed = true;
            if (!memory_semaphore.IsFree(chunk.unbind_tick)) {
                continue;
            }
        }
        vmaFreeMemory(instance.GetAllocator(), chunk.allocation);
        arena_chunk_count.fetch_sub(1, std::memory_order_relaxed);
        arena_chunk_bytes.fetch_sub(ARENA_CHUNK_SIZE, std::memory_order_relaxed);
        if (chunk.system) {
            arena_system_bytes.fetch_sub(ARENA_CHUNK_SIZE, std::memory_order_relaxed);
        }
        ++reclaim_stats.released_chunks;
        arena_chunks.erase(arena_chunks.begin() + i);
        chunk_hint = 0;
    }
}

void BufferCache::RetireRange(const Backing& backing, u64 tick) {
    const u64 blocks = backing.end - backing.start;
    resident_ranges.Subtract(backing.start, backing.end);
    // Shaders reading it through the page table fault from now on, and the fault binds it.
    runtime.FillBuffer(bda_pagetable_buffer.get(), backing.start * sizeof(vk::DeviceAddress),
                       blocks * sizeof(vk::DeviceAddress), 0);
    // The next binding uploads the guest data again.
    memory_tracker->MarkRegionAsCpuModified(backing.start << block_shift, blocks << block_shift);
    retired_ranges.push_back({tick, backing});
    arena_resident_bytes.fetch_sub(blocks << block_shift, std::memory_order_relaxed);
}

void BufferCache::ReleaseRetiredRanges() {
    // Retired in tick order: release up to the first the GPU has not finished.
    size_t done = 0;
    while (done < retired_ranges.size() && scheduler.IsFree(retired_ranges[done].tick)) {
        ++done;
    }
    if (done == 0) {
        return;
    }
    // Parts bound again since, or retired again with other memory, keep their binding.
    IntervalList<Interval> keep;
    for (size_t i = done; i < retired_ranges.size(); ++i) {
        keep.Add({retired_ranges[i].backing.start, retired_ranges[i].backing.end});
    }
    for (size_t i = 0; i < done; ++i) {
        const Backing& backing = retired_ranges[i].backing;
        resident_ranges.ForEachGap(backing.start, backing.end, [&](u64 start, u64 end) {
            keep.ForEachGap(start, end, [&](u64 a, u64 b) {
                // From the owner and every bridge mapping it.
                ForEachArenaBinding(a, b, [&](const Buffer* arena, u64 x, u64 y) {
                    auto it = std::ranges::find(pending_unbinds, arena, &ArenaBinds::arena);
                    ArenaBinds& unbinds =
                        it != pending_unbinds.end() ? *it : pending_unbinds.emplace_back(arena);
                    unbinds.binds.push_back(vk::SparseMemoryBind{
                        .resourceOffset = (x << block_shift) - arena->cpu_addr,
                        .size = (y - x) << block_shift,
                    });
                });
            });
        });
        FreeArenaPiece(backing.memory, backing.offset, backing.end - backing.start);
    }
    retired_ranges.erase(retired_ranges.begin(), retired_ranges.begin() + done);
}

template <typename Pick>
u64 BufferCache::RetireRanges(u64 max_bytes, std::optional<bool> system, u64 tick, Pick&& pick) {
    // Collected first: retiring edits resident_ranges.
    boost::container::small_vector<Backing, 64> picks;
    u64 total = 0;
    for (const Backing& backing : resident_ranges) {
        if (total >= max_bytes) {
            break;
        }
        if (system && IsSystemBacking(backing.memory, backing.offset) != *system) {
            continue;
        }
        for (u64 block = backing.start; block < backing.end && total < max_bytes;) {
            const u64 unit = (block << block_shift) >> UNIT_SHIFT;
            const u64 end =
                std::min<u64>(backing.end, ((unit + 1) << UNIT_SHIFT) >> block_shift);
            const u64 size = (end - block) << block_shift;
            // GPU-written data would need a download first: such ranges stay.
            if (pick(unit_last_use[unit]) &&
                !memory_tracker->IsRegionGpuModified(block << block_shift, size)) {
                const Backing piece = backing.SubRange(block, end);
                Backing* last = picks.empty() ? nullptr : &picks.back();
                if (last && last->end == piece.start && last->CanMergeWith(piece)) {
                    last->end = piece.end;
                } else {
                    picks.push_back(piece);
                }
                total += size;
            }
            block = end;
        }
    }
    for (const Backing& backing : picks) {
        RetireRange(backing, tick);
    }
    return total;
}

bool BufferCache::EmergencyReclaim() {
    // bbport: VRAM, system memory and VMA all refused a chunk. Rather than fail, retire ranges
    // not bound this frame as of the last submission, wait for it, and reuse their memory now.
    // A shader in the submission being recorded that reads one through the page table before
    // its entries are cleared reads unbound memory (zeros on AMD), and data a shader wrote
    // through the page table is lost: wrong output for a while, where the alternative is a crash.
    const u64 tick = scheduler.CurrentTick() - 1;
    if (tick == 0) {
        return false;
    }
    const u32 frame =
        static_cast<u32>(BbStats::gpu_frames.load(std::memory_order_relaxed)) & ~FAULTED_UNIT;
    const auto not_this_frame = [frame](u32 entry) { return (entry & ~FAULTED_UNIT) != frame; };
    u64 retired = RetireRanges(64_MB, false, tick, not_this_frame);
    if (retired == 0) {
        retired = RetireRanges(64_MB, true, tick, not_this_frame);
    }
    if (retired == 0) {
        return false;
    }
    scheduler.Wait(tick);
    ReleaseRetiredRanges();
    reclaim_stats.reclaimed_bytes += retired;
    ++reclaim_stats.emergencies;
    LOG_WARNING(Render_Vulkan, "Out of memory for buffer arenas: reclaimed {} MiB in place; {}",
                retired >> 20, instance.DescribeMemory());
    return true;
}

void BufferCache::TickArenas(const MemoryLevels& levels) {
    ReleaseRetiredRanges();
    const bool pressured = levels.budget != 0 && levels.used >= levels.pressure;
    ReleaseEmptyChunks(pressured);

    // The next VRAM chunk, allocated on the worker while half a chunk is still free.
    u64 vram_free_blocks = 0;
    for (const ArenaChunk& chunk : arena_chunks) {
        vram_free_blocks += chunk.system ? 0 : chunk.free_blocks;
    }
    if (vram_free_blocks < chunk_blocks / 2) {
        std::scoped_lock lock{prefetch.mutex};
        if (!prefetch.requested && !prefetch.allocation && !PrefersSystemMemory()) {
            prefetch.requested = true;
            prefetch.cv.notify_all();
        }
    }
    // bbport: retiring ranges no draw bound for a while is opt-in (BB_ARENA_RECLAIM=1). Shaders
    // also read guest memory through the page table (ReadConst: constants and resource
    // descriptors) without binding it, which leaves no trace here: on an RX 5500M a previous
    // area's ranges looked unused, were retired, and looking back at it smeared its textures
    // (descriptors read from cleared entries). Without it, only an allocation VRAM, system
    // memory and VMA all refuse reclaims (EmergencyReclaim), instead of failing.
    static const bool reclaim = [] {
        const char* env = std::getenv("BB_ARENA_RECLAIM");
        return env && env[0] == '1';
    }();
    if (levels.budget == 0 || !reclaim) {
        return;
    }

    // Frames since the last binding; display frames, so the ages hold at any submission rate.
    const u32 frame =
        static_cast<u32>(BbStats::gpu_frames.load(std::memory_order_relaxed)) & ~FAULTED_UNIT;
    const auto age = [frame](u32 entry) { return (frame - (entry & ~FAULTED_UNIT)) & ~FAULTED_UNIT; };
    const u64 tick = scheduler.CurrentTick();
    if (pressured) {
        // At most once a second (30 fps), a bounded amount: reuploads stay off the frame time.
        if (frame - last_reclaim_frame < 30) {
            return;
        }
        last_reclaim_frame = frame;
        const bool critical = levels.used >= levels.critical;
        // ~10 s unused, ~3 s at the critical mark. Ranges read through the page table stay:
        // their later reads leave no trace here, and a shader may also have written them
        // through it, which the memory tracker does not see (retiring would lose that data).
        const u32 min_age = critical ? 90 : 300;
        const auto cold = [&](u32 entry) {
            return !(entry & FAULTED_UNIT) && age(entry) >= min_age;
        };
        const u64 max_bytes = critical ? 128_MB : 64_MB;
        // VRAM first: that is what is short. Cold ranges in system memory free its chunks.
        u64 retired = RetireRanges(max_bytes, false, tick, cold);
        retired += RetireRanges(max_bytes - std::min(retired, max_bytes), true, tick, cold);
        reclaim_stats.reclaimed_bytes += retired;
        return;
    }
    // VRAM to spare and ranges in system memory: retire those unused for ~10 s, so their chunks
    // empty out and their next binding lands in VRAM. (Ranges in use are left alone: taken back
    // before release, they would only return to the same memory.)
    const u64 system_bytes = arena_system_bytes.load(std::memory_order_relaxed);
    const u64 headroom_mark = levels.budget / 100 * 85;
    if (system_bytes == 0 || levels.used + ARENA_CHUNK_SIZE >= headroom_mark ||
        frame - last_migrate_frame < 60) {
        return;
    }
    last_migrate_frame = frame;
    const u64 max_bytes = std::min<u64>(32_MB, headroom_mark - levels.used - ARENA_CHUNK_SIZE);
    reclaim_stats.migrated_bytes += RetireRanges(max_bytes, true, tick, [&](u32 entry) {
        return !(entry & FAULTED_UNIT) && age(entry) >= 300;
    });
}

void BufferCache::PrefetchLoop() {
    // bbport: also the freeze watchdog. When the GPU thread has made no submission for 5 s it
    // prints the stage that thread last entered (BbWatch), every 5 s while it stays stuck.
    using Clock = std::chrono::steady_clock;
    u64 last_count = 0;
    auto last_change = Clock::now();
    s64 reported = 0;
    std::unique_lock lock{prefetch.mutex};
    while (true) {
        prefetch.cv.wait_for(lock, std::chrono::seconds(1),
                             [this] { return prefetch.stop || prefetch.requested; });
        if (prefetch.stop) {
            return;
        }
        if (!prefetch.requested) {
            // Slow-frame entries VideoOut queued (BbHitch), printed off the flip path and
            // outside the lock the recording thread may wait on.
            lock.unlock();
            BbHitch::PrintPending();
            lock.lock();
            if (prefetch.stop || prefetch.requested) {
                continue;
            }
            const u64 count = BbWatch::submissions.load(std::memory_order_relaxed);
            const auto now = Clock::now();
            if (count != last_count) {
                last_count = count;
                last_change = now;
                reported = 0;
                continue;
            }
            const s64 stalled =
                std::chrono::duration_cast<std::chrono::seconds>(now - last_change).count();
            if (count != 0 && stalled >= 5 && stalled >= reported + 5) {
                reported = stalled;
                std::printf("Watchdog: no GPU submission for %lld s; GPU thread stage: %s; %s\n",
                            static_cast<long long>(stalled),
                            BbWatch::stage.load(std::memory_order_relaxed),
                            instance.DescribeMemory().c_str());
                std::fflush(stdout);
            }
            continue;
        }
        lock.unlock();
        VmaAllocation allocation{};
        VmaAllocationInfo info{};
        const bool ok = AllocateChunkMemory(instance.GetAllocator(), arena_memory_type_index,
                                            ARENA_CHUNK_SIZE, block_size, allocation, info);
        lock.lock();
        if (ok) {
            prefetch.allocation = allocation;
            prefetch.memory = info.deviceMemory;
            prefetch.offset = info.offset;
        }
        prefetch.requested = false;
        prefetch.cv.notify_all();
    }
}

bool BufferCache::AdoptPrefetchedChunk(bool wait) {
    const BbWatch::ScopedStage stage{"waiting for the arena chunk worker"};
    std::unique_lock lock{prefetch.mutex};
    if (wait) {
        // One in progress finishes sooner than a new allocation would.
        prefetch.cv.wait(lock, [this] { return !prefetch.requested; });
    }
    if (!prefetch.allocation) {
        return false;
    }
    AdoptArenaChunk(std::exchange(prefetch.allocation, nullptr), prefetch.memory, prefetch.offset,
                    false);
    return true;
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            copies.emplace_back(total_size_bytes, addr, size);
            total_size_bytes += size;
        },
        [&] { src_buffer = UploadCopies(arena, copies, total_size_bytes); });

    if (src_buffer) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    // bbport: a buffer the GPU writes first takes the newer contents of an image at its address
    // (a render target's live only in the image). The shader may read what it writes, and the
    // image is refreshed from the buffer afterwards: what the write leaves must be the image's.
    if (is_texel_buffer || is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size, is_written);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    BbStats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // bbport: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!BbToggle::Disabled(BbToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            BbCopy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        BbCopy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size,
                                             bool for_write) {
    if (auto type = texture_cache.IsMeta(device_addr); type && !for_write) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    // bbport: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !BbToggle::Disabled(BbToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id =
        texture_cache.FindImageFromRange(device_addr, size, true, !for_write);
    if (!image_id) {
        // Only an address no image starts at is remembered. One whose image is stale for now
        // (dirty, or not GPU-written yet) turns valid without the registry changing: remembered,
        // texel reads of that render target got the buffer's old contents until some image
        // was created or freed (none for minutes at a time on a 4 GB GPU near its budget).
        if (!texture_cache.HasImageAt(device_addr, false)) {
            miss = {device_addr, size, generation};
        }
        return false;
    }
    // bbport: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        // A write covers only its own range: levels past it keep their memory as it is.
        if (for_write && mip_info.offset + mip_info.size > size) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty() && pending_unbinds.empty()) {
        // Pieces freed without needing an unbind (bound again since) have nothing to wait for.
        for (ArenaChunk& chunk : arena_chunks) {
            if (chunk.unbind_pending) {
                chunk.unbind_pending = false;
                chunk.unbind_tick = last_memory_signal;
            }
        }
        return;
    }

    // bbport: unbinds of released ranges travel in the same batch as the binds, which may reuse
    // their memory for other ranges (never the same range: TrimPendingUnbinds), so the batch
    // needs no ordering against another one and waits for nothing.
    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_unbinds.size() + pending_binds.size());
    for (const auto* list : {&pending_unbinds, &pending_binds}) {
        for (const auto& binds : *list) {
            if (binds.binds.empty()) {
                continue;
            }
            buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
                .buffer = binds.arena->Handle(),
                .bindCount = static_cast<u32>(binds.binds.size()),
                .pBinds = binds.binds.data(),
            });
            BbHitch::sparse_binds.fetch_add(binds.binds.size(), std::memory_order_relaxed);
        }
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    // bbport: any failure, not just a lost device. The submission waits for this batch's signal,
    // which a failed batch never gives: the GPU would wait forever (a freeze, not a crash).
    ASSERT_MSG(submit_result == vk::Result::eSuccess, "Sparse binding failed: {}; {}",
               vk::to_string(submit_result), instance.DescribeMemory());

    for (ArenaChunk& chunk : arena_chunks) {
        if (chunk.unbind_pending) {
            chunk.unbind_pending = false;
            chunk.unbind_tick = signal_tick;
        }
    }
    last_memory_signal = signal_tick;
    pending_binds.clear();
    pending_unbinds.clear();
}

} // namespace VideoCore
