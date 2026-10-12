// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

// bbport: what happened during a long frame (VideoOut prints a "Hitch:" line for flips that
// took longer than BB_HITCH_MS, default 50). Counters only ever grow; the reporter prints the
// deltas between flips, so other statistics windows can reset theirs freely.
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
} // namespace BbHitch
