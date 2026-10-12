// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_toggles.h"
#include "bbport_hitch.h"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/types.h"
#include "core/devtools/widget/gpu_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

namespace Vulkan {

using VideoCore::MemoryType;

namespace {

// bbport: staging memory is populated (allocated and cleared) by the kernel on first CPU access,
// ~2 ms per 16 MiB block. Streaming bursts a few seconds apart re-created the blocks trimmed
// in between and copies into them ran at 0.3 GB/s (stutter). Blocks now live 30 s idle, the
// upload ring keeps BB_STAGING_KEEP_MB (512) and is populated at startup.
constexpr u64 RING_IDLE_FRAMES = 3000;
constexpr u64 LARGE_IDLE_FRAMES = 3000;
constexpr u64 PREWARM_BLOCKS = 8;
constexpr u64 LARGE_MIN_GRANULARITY = 64_KB;
constexpr u64 BLOCK_SIZE = 16_MB;
// bbport: device-local staging is VRAM taken from textures and arenas on 4 GB cards. Detiled
// texture data, which took ~600 MiB of it whenever an area loaded, now goes through the tile
// manager's scratch; what is left (copies between images, oversized images) is small. The ring
// keeps two blocks and releases others after ~1 minute (1800 submissions) unused: each block is
// its own 16 MiB driver allocation, so releasing them sooner only made them come back.
constexpr u64 DEVICE_LOCAL_KEEP_BLOCKS = 2;
constexpr u64 DEVICE_LOCAL_IDLE_FRAMES = 1800;

u64 RoundAllocationSize(u64 size) {
    const u64 step = std::max(std::bit_floor(size) / 4, LARGE_MIN_GRANULARITY);
    return Common::AlignUp(size, step);
}

} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const char* env = std::getenv("BB_STAGING_KEEP_MB");
    keep_blocks = (env ? std::strtoull(env, nullptr, 10) : 512) * 1_MB / BLOCK_SIZE;
    Ring& ring = rings[u32(MemoryType::HostUncached)];
    for (u64 i = 0; i < PREWARM_BLOCKS; ++i) {
        auto& block = ring.blocks.emplace_back(Block{
            .buffer = std::make_unique<VideoCore::StreamBuffer>(
                instance, scheduler, MemoryType::HostUncached, BLOCK_SIZE),
            .last_used_frame = frame,
        });
        if (!block.buffer->mapped_data.empty()) {
            std::memset(block.buffer->mapped_data.data(), 0, block.buffer->mapped_data.size());
        }
    }
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(u64 size, MemoryType type, u64 alignment,
                                            bool deferred) {
    Ring& ring = rings[u32(type)];
    if (deferred || size > BLOCK_SIZE) {
        return RequestLarge(size, type, deferred);
    }
    return RequestFromRing(ring, size, alignment, type);
}

StagingBufferRef StagingBufferPool::RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                                    MemoryType type) {
    Block* best{};
    u64 best_offset{};
    // bbport: device-local requests take the first block with room, not the next one after the
    // last used. Rotating through every block kept the 38 a texture-streaming burst created all
    // in use, so none went idle and 608 MiB of a 4 GB GPU's VRAM stayed staging; packed at the
    // front, the blocks the burst added go idle and are trimmed.
    // A block that failed in this submission is skipped for the rest of it: in a streaming
    // burst every request would otherwise retry every busy block, each retry a GPU progress
    // query.
    const bool packed = type == MemoryType::DeviceLocal;
    const size_t first = packed ? 0 : ring.current;
    const u64 tick = scheduler.CurrentTick();
    for (size_t i = 0; i < ring.blocks.size(); ++i) {
        const size_t index = (first + i) % ring.blocks.size();
        Block* block = &ring.blocks[index];
        if (packed && block->failed_tick == tick) {
            continue;
        }
        if (const auto offset = block->buffer->Reserve(size, alignment, false)) {
            ring.current = index;
            best = block;
            best_offset = *offset;
            break;
        }
        block->failed_tick = tick;
    }

    if (!best) {
        BbHitch::staging_blocks.fetch_add(1, std::memory_order_relaxed);
        best = &ring.blocks.emplace_back(Block{
            .buffer =
                std::make_unique<VideoCore::StreamBuffer>(instance, scheduler, type, BLOCK_SIZE),
            .last_used_frame = frame,
        });
        ring.current = ring.blocks.size() - 1;
        best_offset = *best->buffer->Reserve(size, alignment, false);
    }

    auto& buffer = *best->buffer;
    best->last_used_frame = frame;

    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = best_offset,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data() + best_offset;
    }
    return ref;
}

StagingBufferRef StagingBufferPool::RequestLarge(u64 size, MemoryType type, bool deferred) {
    auto& cache = large_caches[u32(type)];
    const u64 max_size = std::max(size + size / 4, RoundAllocationSize(size));

    LargeBuffer* best{};
    for (LargeBuffer& entry : cache) {
        const u64 entry_size = entry.buffer->SizeBytes();
        if (entry.held || entry_size < size || entry_size > max_size ||
            !scheduler.IsFree(entry.tick)) {
            continue;
        }
        if (!best || entry_size < best->buffer->SizeBytes()) {
            best = &entry;
        }
    }

    if (!best) {
        BbHitch::staging_blocks.fetch_add(1, std::memory_order_relaxed);
        best = &cache.emplace_back(LargeBuffer{
            .buffer = std::make_unique<VideoCore::Buffer>(instance, 0, RoundAllocationSize(size),
                                                          type, "StagingBufferPool:Dedicated"),
            .tick = 0,
            .last_used_frame = frame,
            .held = false,
        });
    }

    best->tick = scheduler.CurrentTick();
    best->last_used_frame = frame;
    best->held = deferred;

    VideoCore::Buffer& buffer = *best->buffer;
    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = 0,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data();
    }
    return ref;
}

void StagingBufferPool::FreeDeferred(const StagingBufferRef& ref) {
    auto& cache = large_caches[u32(ref.type)];
    const auto it = std::ranges::find_if(
        cache, [&](const LargeBuffer& entry) { return entry.buffer.get() == ref.buffer; });
    ASSERT_MSG(it != cache.end() && it->held, "FreeDeferred on an allocation that is not held");
    it->held = false;
    it->tick = scheduler.CurrentTick();
    it->last_used_frame = frame;
}

void StagingBufferPool::TickFrame() {
    ++frame;
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        const bool device_local = MemoryType(i) == MemoryType::DeviceLocal;
        TrimRing(rings[i], device_local ? DEVICE_LOCAL_KEEP_BLOCKS : keep_blocks,
                 device_local ? DEVICE_LOCAL_IDLE_FRAMES : RING_IDLE_FRAMES);
        TrimLarge(large_caches[i], device_local ? DEVICE_LOCAL_IDLE_FRAMES : LARGE_IDLE_FRAMES);
    }
    PublishStats();
}

u64 StagingBufferPool::ReleaseIdle() {
    // Device-local ring blocks but one, and every dedicated buffer, that no pending submission
    // uses. The host upload ring stays: it is populated at startup and slow to populate again.
    const auto release = [this] {
        u64 freed = TrimRing(rings[u32(MemoryType::DeviceLocal)], 1, 0);
        for (auto& cache : large_caches) {
            freed += TrimLarge(cache, 0);
        }
        return freed;
    };
    u64 freed = release();
    if (freed == 0 && scheduler.CurrentTick() > 1) {
        // What the submitted work holds is free once it completes.
        scheduler.WaitSubmitted();
        freed = release();
    }
    if (freed != 0) {
        std::printf("Staging: VRAM refused an allocation; released %llu MiB of idle staging\n",
                    static_cast<unsigned long long>(freed >> 20));
    }
    return freed;
}

u64 StagingBufferPool::DeviceLocalRingBytes() const {
    u64 bytes = 0;
    for (const Block& block : rings[u32(MemoryType::DeviceLocal)].blocks) {
        bytes += block.buffer->SizeBytes();
    }
    return bytes;
}

u64 StagingBufferPool::DeviceLocalBytes() const {
    u64 bytes = DeviceLocalRingBytes();
    for (const LargeBuffer& entry : large_caches[u32(MemoryType::DeviceLocal)]) {
        bytes += entry.buffer->SizeBytes();
    }
    return bytes;
}

u64 StagingBufferPool::TrimRing(Ring& ring, u64 keep, u64 idle_frames) {
    auto& blocks = ring.blocks;
    u64 freed = 0;
    for (size_t i = blocks.size(); i-- > 0;) {
        const Block& block = blocks[i];
        if (blocks.size() <= keep || frame - block.last_used_frame < idle_frames ||
            !scheduler.IsFree(block.buffer->LastTick())) {
            continue;
        }
        freed += block.buffer->SizeBytes();
        blocks.erase(blocks.begin() + i);
        if (i < ring.current) {
            --ring.current;
        } else if (i == ring.current) {
            ring.current = 0;
        }
    }
    if (ring.current >= blocks.size()) {
        ring.current = 0;
    }
    return freed;
}

u64 StagingBufferPool::TrimLarge(std::vector<LargeBuffer>& cache, u64 idle_frames) {
    u64 freed = 0;
    std::erase_if(cache, [this, idle_frames, &freed](const LargeBuffer& entry) {
        const bool idle = !entry.held && frame - entry.last_used_frame >= idle_frames &&
                          scheduler.IsFree(entry.tick);
        freed += idle ? entry.buffer->SizeBytes() : 0;
        return idle;
    });
    return freed;
}

void StagingBufferPool::PublishStats() const {
    using namespace Core::Devtools::Widget;
    if (!GpuMemoryViewer::IsEnabled()) {
        return;
    }
    static constexpr std::array<const char*, NUM_TYPES> names = {"DeviceLocal", "HostUncached",
                                                                 "HostCached"};
    GpuMemoryGroup group{.name = "Staging"};
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        u64 bytes = 0;
        for (const Block& block : rings[i].blocks) {
            bytes += block.buffer->SizeBytes();
        }
        for (const LargeBuffer& entry : large_caches[i]) {
            bytes += entry.buffer->SizeBytes();
        }
        if (bytes != 0) {
            group.rows.push_back({.name = names[i], .bytes = bytes});
        }
    }
    GpuMemoryViewer::Publish(std::move(group));
}

} // namespace Vulkan
