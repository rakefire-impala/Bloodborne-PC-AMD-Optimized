// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace Vulkan {

// bbport: when FSR frame generation steps aside for video memory (Presenter::GuardFgVram),
// decided every ~0.5 s from the main heap's usage and budget, the real frames since the
// previous check and what the texture cache did meanwhile. Paused, it gives its frame images
// (~120 MiB at 1080p) back to the textures and keeps its FI/OF context (freeing that at the
// budget blocked in the driver for 9-16 s, Oct 21).
//
// Usage near the budget is no reason on its own: on a 4 GB GPU the texture cache keeps the main
// heap at 90-95% of its budget in busy scenes. It pauses when, for 1.5 s,
// - Windows pages the game's memory: under 10 real FPS near the budget (340 ms frames, 3 FPS
//   for good, with another program holding ~760 MiB; 2.4 FPS in a heavy Old Yharnam street),
// - or the textures no longer fit: the cache evicts images it used within ~10 s, over 64 MiB/s
//   (Oct 24: 140-460 MiB/s in an Old Yharnam fight and after travel, every resident image used
//   within a second). That came seconds before the paging there, which FG's memory (~245 MiB
//   of a ~2.8 GB budget) tipped over.
//
// It resumes after 10 s of normal frames, no such evictions, no texture collection under memory
// pressure and room for its images inside VMA's blocks: what they hold in use (`used`, the
// texture cache's measure) at most 85% of the budget. Without that last part every try paged
// again (2.6-2.7 FPS, 5 tries 10-160 s apart at 2709 of 2803 MiB, Oct 24): freeing FG's images
// had taken the cache off its pressure mark. Resuming on normal frames alone did the same (2.4
// FPS, 5 tries). A pause soon after resuming doubles the wait, up to 160 s.
//
// Paging also counts from 90% of the budget (was 95%): one collapse began at 94.3% and ran
// ~10 s at under 1 FPS before the budget dropped and it counted.
//
// - or video memory is full: 96% of the budget. FG on paged at 95.5-97.4% (four collapses on
//   an RX 5500M, Oct 24) and held at 95-96.3%; stepping aside there spares the collapse.
class FgVramGuard {
public:
    enum class Action { None, Pause, Resume };
    /// Why the last pause came.
    enum class Reason { SlowFrames, TexturesDoNotFit, MemoryFull };

    struct Limits {
        uint64_t percent = 90; ///< of the budget: near it (BB_FG_VRAM_PAUSE_PERCENT)
        double slow_fps = 10.0; ///< real frames per second under which it is paging (BB_FG_VRAM_PAUSE_FPS)
        uint64_t thrash_percent = 90; ///< of the budget: textures evicted back count from here
        double thrash_mib = 64.0; ///< per second of images evicted soon after use (BB_FG_VRAM_THRASH_MIB)
        uint64_t resume_percent = 85; ///< of the budget: `used` at most this to resume
        uint64_t full_percent = 96; ///< of the budget: video memory full (BB_FG_VRAM_FULL_PERCENT)
    };
    Limits limits;

    /// One check: the main heap's usage and budget now, the real frames over the seconds since
    /// the previous check, whether the texture cache collected under pressure meanwhile, the
    /// bytes of images it evicted within ~10 s of their last use, and what VMA's blocks hold in
    /// use (usage minus the space free inside them; the usage when not given).
    Action Check(uint64_t usage, uint64_t budget, uint32_t frames, double seconds, bool pressured,
                 uint64_t hot_evicted = 0, uint64_t used = ~0ull) {
        if (used == ~0ull) {
            used = usage;
        }
        if (!budget || seconds <= 0.0) {
            return Action::None;
        }
        const bool slow = frames < limits.slow_fps * seconds;
        const bool thrash = double(hot_evicted) >= limits.thrash_mib * seconds * (1 << 20);
        const bool paging = slow && usage * 100 > budget * limits.percent;
        const bool evicting = thrash && usage * 100 > budget * limits.thrash_percent;
        const bool full = usage * 100 >= budget * limits.full_percent;
        if (!paused) {
            running += seconds;
            pause_checks = paging || evicting || full ? pause_checks + 1 : 0;
            thrash_checks = evicting ? thrash_checks + 1 : 0;
            full_checks = full ? full_checks + 1 : 0;
            if (pause_checks < PauseAfter) {
                return Action::None;
            }
            reason = thrash_checks >= PauseAfter ? Reason::TexturesDoNotFit
                     : full_checks >= PauseAfter ? Reason::MemoryFull
                                                 : Reason::SlowFrames;
            pause_checks = thrash_checks = full_checks = clear_checks = 0;
            resume_after = resumed && running < RepeatWithin
                               ? std::min(resume_after * 2, MaxResumeAfter)
                               : ResumeAfter;
            paused = true;
            return Action::Pause;
        }
        const bool room = used * 100 <= budget * limits.resume_percent;
        clear_checks = slow || pressured || thrash || !room ? 0 : clear_checks + 1;
        if (clear_checks < resume_after) {
            return Action::None;
        }
        clear_checks = 0;
        paused = false;
        resumed = true;
        running = 0.0;
        return Action::Resume;
    }

    /// Frame generation turned off: a later pause starts again from the shortest wait.
    void Reset() {
        const Limits kept = limits;
        *this = FgVramGuard{};
        limits = kept;
    }

    bool Paused() const {
        return paused;
    }
    Reason LastReason() const {
        return reason;
    }
    /// Checks in a row with normal frames, no pressure and room that the current pause waits
    /// for (2 a second).
    uint32_t ResumeChecks() const {
        return resume_after;
    }

private:
    static constexpr uint32_t PauseAfter = 3;      // 1.5 s
    static constexpr uint32_t ResumeAfter = 20;    // 10 s
    static constexpr uint32_t MaxResumeAfter = 320; // 160 s
    static constexpr double RepeatWithin = 60.0;   // seconds of FG between a resume and a pause

    bool paused = false, resumed = false;
    Reason reason = Reason::SlowFrames;
    uint32_t pause_checks = 0, thrash_checks = 0, full_checks = 0, clear_checks = 0;
    uint32_t resume_after = ResumeAfter;
    double running = 0.0; ///< seconds since the last resume
};

} // namespace Vulkan
