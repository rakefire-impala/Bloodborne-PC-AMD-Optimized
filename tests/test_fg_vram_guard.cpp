// SPDX-License-Identifier: GPL-2.0-or-later
// When FSR frame generation steps aside for video memory, without a GPU or game image.
#include <cassert>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/fg_vram_guard.h"

using Vulkan::FgVramGuard;
using Action = FgVramGuard::Action;
using Reason = FgVramGuard::Reason;

constexpr uint64_t MiB = 1ull << 20;

// Checks of half a second at a steady frame rate; the first action other than None.
// `used_mib`: what VMA's blocks hold in use (the usage when not given).
static Action Run(FgVramGuard& guard, int checks, uint64_t usage, uint64_t budget, uint32_t frames,
                  int* at = nullptr, bool pressured = false, uint64_t hot_mib = 0,
                  uint64_t used_mib = 0) {
    for (int i = 0; i < checks; ++i) {
        const Action action = guard.Check(usage * MiB, budget * MiB, frames, 0.5, pressured,
                                          hot_mib * MiB, used_mib ? used_mib * MiB : ~0ull);
        if (action != Action::None) {
            if (at) *at = i + 1;
            return action;
        }
    }
    return Action::None;
}

int main() {
    // A hunter NPC fight on an RX 5500M (Oct 24 log): the main heap at 94.7-95.4% of its
    // budget, ~20 real FPS with a ~185 ms stall every second or two. The old guard paused at
    // 95% for the rest of the session; frame generation stays on.
    {
        FgVramGuard guard;
        assert(Run(guard, 360, 2654, 2802, 10) == Action::None);
        for (int i = 0; i < 360; ++i) {
            assert(guard.Check(2680 * MiB, 2810 * MiB, i % 3 ? 10 : 6, 0.5, false) == Action::None);
        }
        assert(!guard.Paused());
        // From 96% it steps aside: FG paged there in four runs (95.5-97.4%).
        int at = 0;
        assert(Run(guard, 10, 2719, 2810, 10, &at) == Action::Pause && at == 3);
        assert(guard.LastReason() == Reason::MemoryFull);
    }
    // Slow frames with room in VRAM (loading, a heavy scene on a weak GPU): not paging.
    {
        FgVramGuard guard;
        assert(Run(guard, 100, 1800, 2802, 2) == Action::None);
    }
    // Paging: 95% of a 2516 MiB budget and 340 ms frames (2 frames in each 0.68 s check).
    {
        FgVramGuard guard;
        assert(guard.Check(2390 * MiB, 2516 * MiB, 2, 0.68, false) == Action::None);
        assert(guard.Check(2390 * MiB, 2516 * MiB, 2, 0.68, false) == Action::None);
        assert(guard.Check(2390 * MiB, 2516 * MiB, 2, 0.68, false) == Action::Pause);
        assert(guard.Paused() && guard.ResumeChecks() == 20);
        assert(guard.LastReason() == Reason::SlowFrames);
        // Still paging: stays paused.
        assert(Run(guard, 200, 2390, 2516, 2) == Action::None);
        // Normal frames again, but VMA's blocks hold as much in use: no room, stays paused.
        assert(Run(guard, 200, 2390, 2516, 15) == Action::None);
        // Room inside the blocks (the textures gave some back): resumes after 10 s, and a slow
        // check in between starts the 10 s over.
        assert(Run(guard, 15, 2390, 2516, 15, nullptr, false, 0, 2000) == Action::None);
        assert(guard.Check(2390 * MiB, 2516 * MiB, 1, 0.5, false, 0, 2000 * MiB) == Action::None);
        int at = 0;
        assert(Run(guard, 30, 2390, 2516, 15, &at, false, 0, 2000) == Action::Resume && at == 20);
        assert(!guard.Paused());
    }
    // Paused in a heavy street (Oct 24, second run): frames normal while paused, the texture
    // cache collecting under pressure throughout. Resuming there brought 2.4 FPS back on every
    // try, so it waits for a stretch without pressure, however long.
    {
        FgVramGuard guard;
        assert(Run(guard, 3, 2718, 2845, 1) == Action::Pause);
        assert(Run(guard, 600, 2718, 2845, 25, nullptr, true, 0, 2300) == Action::None); // 5 min
        for (int i = 0; i < 105; ++i) { // pressure now and then: never 10 s without it
            assert(guard.Check(2718 * MiB, 2845 * MiB, 25, 0.5, i % 15 == 14, 0, 2300 * MiB) ==
                   Action::None);
        }
        int at = 0;
        assert(Run(guard, 40, 2718, 2845, 25, &at, false, 0, 2300) == Action::Resume && at == 20);
        // Pressure while running does not pause by itself (normal frames, under 96%).
        assert(Run(guard, 200, 2718, 2845, 25, nullptr, true) == Action::None);
    }
    // The third run (Oct 24, 22:30): FG's images freed took the cache off its pressure mark, so
    // no pressure while paused; yet 5 tries paged again at 2709 of 2803 MiB (2.6-2.7 FPS). VMA's
    // blocks still held ~86% in use: no room for FG's images, so it stays paused now.
    {
        FgVramGuard guard;
        assert(Run(guard, 3, 2709, 2803, 1) == Action::Pause);
        assert(Run(guard, 600, 2709, 2803, 30, nullptr, false, 0, 2420) == Action::None);
        int at = 0;
        assert(Run(guard, 40, 2709, 2803, 30, &at, false, 0, 2350) == Action::Resume && at == 20);
    }
    // A collapse at 94.3% of the budget (2644 of 2805 MiB, 0.9 FPS) ran ~10 s while the guard
    // waited for 95%; it counts from 90% now.
    {
        FgVramGuard guard;
        int at = 0;
        assert(Run(guard, 10, 2644, 2805, 0, &at) == Action::Pause && at == 3);
        assert(guard.LastReason() == Reason::SlowFrames);
        FgVramGuard slow_scene;
        assert(Run(slow_scene, 10, 2400, 2805, 0) == Action::None); // 85.6%: slow, not paging
    }
    // Textures evicted straight back (Oct 24, after travel back to Old Yharnam): 2565 of
    // 2777 MiB, ~240 MiB/s of images evicted within ~10 s of use, frames still normal. Paging
    // (2.6 FPS) followed seconds later; the guard steps aside before it.
    {
        FgVramGuard guard;
        int at = 0;
        assert(Run(guard, 10, 2565, 2777, 25, &at, true, 120) == Action::Pause && at == 3);
        assert(guard.LastReason() == Reason::TexturesDoNotFit);
        // Still evicting straight back, even with normal frames and no pressure flag: paused.
        assert(Run(guard, 100, 2565, 2777, 30, nullptr, false, 40, 2300) == Action::None);
        // The evictions stop (room again, e.g. a lighter area): back after 10 s.
        assert(Run(guard, 40, 2400, 2777, 30, &at, false, 0, 2300) == Action::Resume && at == 20);
        // Video memory full, frames normal: steps aside before paging.
        assert(Run(guard, 3, 2700, 2777, 30) == Action::Pause);
        assert(guard.LastReason() == Reason::MemoryFull);
    }
    // Evictions below 64 MiB/s, or with room (below 90% of the budget), are ordinary upkeep.
    {
        FgVramGuard guard;
        assert(Run(guard, 200, 2565, 2777, 25, nullptr, true, 30) == Action::None);
        assert(Run(guard, 200, 2400, 2777, 25, nullptr, true, 200) == Action::None);
        // Neither alone: three in a row of any reason.
        for (int i = 0; i < 60; ++i) {
            const uint64_t hot = i % 3 == 2 ? 0 : 120;
            assert(guard.Check(2565 * MiB, 2777 * MiB, 25, 0.5, true, hot * MiB) == Action::None);
        }
        int at = 0;
        assert(guard.Check(2565 * MiB, 2777 * MiB, 25, 0.5, true, 120 * MiB) == Action::None);
        assert(Run(guard, 3, 2620, 2777, 1, &at) == Action::Pause && at == 2); // mixed: in a row
        assert(guard.LastReason() == Reason::SlowFrames);
    }
    // The checks near the budget have to be in a row (94.6%: under the full mark).
    {
        FgVramGuard guard;
        for (int i = 0; i < 50; ++i) {
            const uint32_t frames = i % 3 == 2 ? 15 : 2;
            assert(guard.Check(2380 * MiB, 2516 * MiB, frames, 0.5, false) == Action::None);
        }
    }
    // Paging again soon after resuming: each pause waits twice as long, up to 160 s; after a
    // minute of frame generation the next pause waits 10 s again.
    {
        FgVramGuard guard;
        assert(Run(guard, 3, 2380, 2516, 2) == Action::Pause);
        uint32_t expected = 20;
        for (int round = 0; round < 6; ++round) {
            int at = 0;
            assert(Run(guard, 400, 2380, 2516, 15, &at, false, 0, 2000) == Action::Resume);
            assert(at == int(expected));
            assert(Run(guard, 3, 2380, 2516, 2) == Action::Pause);
            expected = expected * 2 > 320 ? 320 : expected * 2;
            assert(guard.ResumeChecks() == expected);
        }
        assert(guard.ResumeChecks() == 320);
        assert(Run(guard, 320, 2380, 2516, 15, nullptr, false, 0, 2000) == Action::Resume);
        assert(Run(guard, 130, 2380, 2516, 15) == Action::None); // 65 s of frame generation
        assert(Run(guard, 3, 2380, 2516, 2) == Action::Pause);
        assert(guard.ResumeChecks() == 20);
    }
    // Frame generation turned off: unpaused, shortest wait, the test limits kept.
    {
        FgVramGuard guard;
        guard.limits.percent = 40;
        guard.limits.slow_fps = 1000.0;
        assert(Run(guard, 3, 1300, 2802, 30) == Action::Pause);
        assert(Run(guard, 3, 1300, 2802, 30) == Action::None); // never "normal" at 1000 FPS
        guard.Reset();
        assert(!guard.Paused() && guard.ResumeChecks() == 20);
        assert(guard.limits.percent == 40 && guard.limits.slow_fps == 1000.0);
        assert(Run(guard, 3, 1300, 2802, 30) == Action::Pause);
    }
    // No budget reported, or no time passed: nothing decided.
    {
        FgVramGuard guard;
        for (int i = 0; i < 10; ++i) {
            assert(guard.Check(2440 * MiB, 0, 0, 0.5, false) == Action::None);
            assert(guard.Check(2440 * MiB, 2516 * MiB, 0, 0.0, false) == Action::None);
        }
        assert(!guard.Paused());
    }
    std::puts("fg vram guard: ok");
    return 0;
}
