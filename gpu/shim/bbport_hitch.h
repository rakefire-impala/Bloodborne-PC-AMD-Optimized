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
inline std::atomic<std::uint64_t> arena_chunks{0}, sparse_binds{0}, bind_ns{0};
inline std::atomic<std::uint64_t> staging_blocks{0}, vram_fallbacks{0};
/// Longest gap between two of the game's command buffer submissions since the last flip (ns),
/// and the time of the last one: a long gap with an idle GPU is the game's CPU side.
inline std::atomic<std::uint64_t> submit_gap_max_ns{0};
inline std::atomic<std::int64_t> last_submit_ns{0};

/// The game's submitting thread's blocking calls since the last flip: total, longest, its name.
inline std::atomic<std::uint64_t> block_ns{0}, block_max_ns{0};
inline std::atomic<const char*> block_max_name{"-"};
inline thread_local bool submit_thread = false;

/// Times a blocking call made by the game; counted when the submitting thread made it.
struct Block {
    const char* name;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit Block(const char* name_) : name{name_} {}
    ~Block() {
        if (!submit_thread) {
            return;
        }
        const std::uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
        block_ns.fetch_add(ns, std::memory_order_relaxed);
        if (ns > block_max_ns.load(std::memory_order_relaxed)) {
            block_max_ns.store(ns, std::memory_order_relaxed);
            block_max_name.store(name, std::memory_order_relaxed);
        }
    }
};

/// Game thread: one command buffer submission.
inline void NoteSubmit() {
    submit_thread = true;
    const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    const std::int64_t last = last_submit_ns.exchange(now, std::memory_order_relaxed);
    if (last == 0) {
        return;
    }
    const std::uint64_t gap = static_cast<std::uint64_t>(now - last);
    std::uint64_t max = submit_gap_max_ns.load(std::memory_order_relaxed);
    while (gap > max &&
           !submit_gap_max_ns.compare_exchange_weak(max, gap, std::memory_order_relaxed)) {
    }
}

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
    double frame_ms, image_mb, buffer_mb, evicted_mb, compile_ms, gpu_wait_ms, copy_wait_ms, bind_ms,
        submit_gap_ms, fault_ms, block_ms, block_max_ms;
    const char* block_name;
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
                    "%llu sparse binds (%.1f ms), %llu staging allocations, %llu VRAM fallbacks; longest "
                    "gap between game submissions %.1f ms, %.1f ms in fault handlers; the "
                    "submitting thread blocked %.1f ms (longest %.1f ms in %s)\n",
                    r.frame_ms, r.image_mb, static_cast<unsigned long long>(r.new_images),
                    r.buffer_mb, static_cast<unsigned long long>(r.evictions), r.evicted_mb,
                    static_cast<unsigned long long>(r.compiles), r.compile_ms, r.gpu_wait_ms,
                    r.copy_wait_ms, static_cast<unsigned long long>(r.arena_chunks),
                    static_cast<unsigned long long>(r.sparse_binds), r.bind_ms,
                    static_cast<unsigned long long>(r.staging),
                    static_cast<unsigned long long>(r.fallbacks), r.submit_gap_ms, r.fault_ms,
                    r.block_ms, r.block_max_ms, r.block_name);
    }
    if (not_shown != 0) {
        std::printf("Hitch: %u more slow frames not shown\n", not_shown);
    }
    if (!records.empty() || not_shown != 0) {
        std::fflush(stdout);
    }
}
} // namespace BbHitch
