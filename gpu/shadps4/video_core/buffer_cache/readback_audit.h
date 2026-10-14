// bbport: BB_READBACK_AUDIT=1 — what GPU readbacks Off gives up, as input for making Off safe.
// With readbacks Off a guest write into a 4 KiB page holding GPU-written data does not download
// that data first, so the next upload of the page puts the stale guest copy of the rest of the
// page over it (the motion blur smear after area loads is suspected to be this). Counted here:
//   - writes: guest write faults into such pages (the skipped downloads),
//   - unmaps: such pages unmapped (Relaxed downloads them into memory being unmapped),
//   - stale uploads: uploads of pages that are CPU- and GPU-modified at once, the actual
//     overwrites (expected 0 with readbacks Relaxed or Precise).
// Printed every 5 s while events happen, with the 1 MiB guest ranges that had the most bytes.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "common/types.h"
#include "core/emulator_settings.h"

namespace VideoCore::ReadbackAudit {

enum class Kind { Write, Unmap, StaleUpload };
constexpr size_t NumKinds = 3;

/// Guest ranges (address, bytes), collected under a region lock and recorded after it.
using Ranges = std::vector<std::pair<VAddr, u64>>;

inline bool Enabled() {
    static const bool enabled = EmulatorSettingsImpl::Flag("BB_READBACK_AUDIT", false);
    return enabled;
}

struct State {
    std::mutex mutex;
    std::array<u64, NumKinds> events{};
    std::array<u64, NumKinds> bytes{};
    std::array<std::map<u64, u64>, NumKinds> by_mib{}; ///< 1 MiB index -> bytes
    std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
};

inline State& GetState() {
    static State state;
    return state;
}

inline void Print(State& s, double seconds) {
    static constexpr const char* names[NumKinds] = {"writes into", "unmaps of",
                                                    "stale uploads over"};
    const char* mode = "Off";
    switch (EmulatorSettings.GetReadbacksMode()) {
    case GpuReadbacksMode::Disabled:
        break;
    case GpuReadbacksMode::Precise:
        mode = "Precise";
        break;
    default:
        mode = "Relaxed";
        break;
    }
    std::printf("Readback audit (%.1f s, readbacks %s):", seconds, mode);
    for (size_t k = 0; k < NumKinds; ++k) {
        std::printf("%s %llu %s GPU-written pages (%.1f MiB)", k ? "," : "",
                    static_cast<unsigned long long>(s.events[k]), names[k],
                    s.bytes[k] / 1048576.0);
    }
    std::printf("\n");
    for (size_t k = 0; k < NumKinds; ++k) {
        if (s.by_mib[k].empty()) {
            continue;
        }
        std::vector<std::pair<u64, u64>> top(s.by_mib[k].begin(), s.by_mib[k].end());
        const size_t shown = std::min<size_t>(top.size(), 8);
        std::partial_sort(top.begin(), top.begin() + shown, top.end(),
                          [](const auto& a, const auto& b) { return a.second > b.second; });
        std::printf("  %s:", names[k]);
        for (size_t i = 0; i < shown; ++i) {
            std::printf(" 0x%llx (%llu KiB)", static_cast<unsigned long long>(top[i].first << 20),
                        static_cast<unsigned long long>(top[i].second >> 10));
        }
        if (top.size() > shown) {
            std::printf(" and %zu more MiB", top.size() - shown);
        }
        std::printf("\n");
    }
    std::fflush(stdout);
}

/// Prints and restarts the window after 5 s; does nothing for a window without events.
inline void MaybePrint(State& s) {
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - s.window_start).count();
    if (seconds < 5.0) {
        return;
    }
    if (s.events[0] + s.events[1] + s.events[2] != 0) {
        Print(s, seconds);
    }
    s.events = {};
    s.bytes = {};
    for (auto& map : s.by_mib) {
        map.clear();
    }
    s.window_start = now;
}

/// The stale uploads of the last upload on this thread, for the caller to name what they hit.
inline Ranges& LastStale() {
    thread_local Ranges ranges;
    return ranges;
}

/// At most 24 descriptions per 5 s window (the window's print resets the count).
inline bool MayDescribe() {
    State& s = GetState();
    std::scoped_lock lk{s.mutex};
    return s.events[static_cast<size_t>(Kind::StaleUpload)] <= 24;
}

inline void Record(Kind kind, const Ranges& ranges) {
    if (kind == Kind::StaleUpload) {
        LastStale() = ranges;
    }
    if (ranges.empty()) {
        return;
    }
    State& s = GetState();
    std::scoped_lock lk{s.mutex};
    const size_t k = static_cast<size_t>(kind);
    ++s.events[k];
    for (const auto& [address, size] : ranges) {
        s.bytes[k] += size;
        s.by_mib[k][address >> 20] += size;
    }
    MaybePrint(s);
}

/// Called once per display frame, so the last window before a quiet period is printed too.
inline void Tick() {
    if (!Enabled()) {
        return;
    }
    // BB_READBACKS_SWITCH=<file>: readbacks are Relaxed while the file exists (checked once a
    // second), to tell a smear that is renewed every frame from one written once.
    static const char* switch_file = std::getenv("BB_READBACKS_SWITCH");
    if (switch_file) {
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(1)) {
            last = now;
            std::FILE* f = std::fopen(switch_file, "rb");
            const int mode = f ? int(GpuReadbacksMode::Relaxed) : -1;
            if (f) {
                std::fclose(f);
            }
            auto& live = EmulatorSettings.readbacks_override;
            if (live.exchange(mode) != mode) {
                std::printf("Readback audit: readbacks %s by %s\n", f ? "Relaxed" : "as configured",
                            switch_file);
            }
        }
    }
    State& s = GetState();
    std::scoped_lock lk{s.mutex};
    MaybePrint(s);
}

} // namespace VideoCore::ReadbackAudit
