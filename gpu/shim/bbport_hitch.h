// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

// bbport: what happened during a long frame. VideoOut records a "Hitch:" entry for flips that
// took longer than BB_HITCH_MS (default 50); the buffer cache's watchdog thread prints them, so
// the flip path does no console I/O. Counters only ever grow; entries carry the deltas between
// flips, so other statistics windows can reset theirs freely.
namespace BbHitch {
inline std::atomic<std::uint64_t> evictions{0}, evicted_bytes{0};
inline std::atomic<std::uint64_t> compiles{0}, compile_ns{0};
inline std::atomic<std::uint64_t> gpu_wait_ns{0}, copy_wait_ns{0};
inline std::atomic<std::uint64_t> arena_chunks{0}, sparse_binds{0};
inline std::atomic<std::uint64_t> staging_blocks{0}, vram_fallbacks{0};

/// Adds the scope's wall time to `total` (ns).
struct Timer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~Timer() {
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count(),
                        std::memory_order_relaxed);
    }
};

struct Record {
    double frame_ms, image_mb, buffer_mb, evicted_mb, compile_ms, gpu_wait_ms, copy_wait_ms;
    std::uint64_t new_images, evictions, compiles, arena_chunks, sparse_binds, staging, fallbacks;
};

struct Queue {
    std::mutex mutex;
    std::vector<Record> records;
    std::uint32_t not_shown = 0;
};
inline Queue queue;

/// Flip thread: queues a record (at most 16 waiting; the rest are only counted).
inline void Push(const Record& record) {
    std::scoped_lock lock{queue.mutex};
    if (queue.records.size() < 16) {
        queue.records.push_back(record);
    } else {
        ++queue.not_shown;
    }
}

/// Counts a slow frame that is not recorded (rate limit).
inline void Skip() {
    std::scoped_lock lock{queue.mutex};
    ++queue.not_shown;
}

/// Watchdog thread: prints and clears what was queued.
inline void PrintPending() {
    std::vector<Record> records;
    std::uint32_t not_shown;
    {
        std::scoped_lock lock{queue.mutex};
        records.swap(queue.records);
        not_shown = std::exchange(queue.not_shown, 0);
    }
    for (const Record& r : records) {
        std::printf("Hitch: %.0f ms frame; %.1f MB image uploads (%llu new images), %.1f MB buffer "
                    "uploads; %llu images evicted (%.1f MB); %llu shader compiles (%.1f ms); "
                    "waited %.1f ms for the GPU, %.1f ms for guest copies; %llu arena chunks, "
                    "%llu sparse binds, %llu staging allocations, %llu VRAM fallbacks\n",
                    r.frame_ms, r.image_mb, static_cast<unsigned long long>(r.new_images),
                    r.buffer_mb, static_cast<unsigned long long>(r.evictions), r.evicted_mb,
                    static_cast<unsigned long long>(r.compiles), r.compile_ms, r.gpu_wait_ms,
                    r.copy_wait_ms, static_cast<unsigned long long>(r.arena_chunks),
                    static_cast<unsigned long long>(r.sparse_binds),
                    static_cast<unsigned long long>(r.staging),
                    static_cast<unsigned long long>(r.fallbacks));
    }
    if (not_shown != 0) {
        std::printf("Hitch: %u more slow frames not shown\n", not_shown);
    }
    if (!records.empty() || not_shown != 0) {
        std::fflush(stdout);
    }
}
} // namespace BbHitch
