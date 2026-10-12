// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

// bbport: freeze diagnosis. The GPU thread counts its submissions and names the stage it is in;
// a watchdog thread (in the buffer cache) prints that stage when the count stops moving, which
// tells a hang in the emulator apart from one in the driver or the guest.
namespace BbWatch {
inline std::atomic<std::uint64_t> submissions{0};
inline std::atomic<const char*> stage{"start"};

inline void Stage(const char* name) noexcept {
    stage.store(name, std::memory_order_relaxed);
}

/// Sets the stage for a scope and restores the previous one after it.
class ScopedStage {
public:
    explicit ScopedStage(const char* name) noexcept
        : previous{stage.exchange(name, std::memory_order_relaxed)} {}
    ~ScopedStage() {
        stage.store(previous, std::memory_order_relaxed);
    }
    ScopedStage(const ScopedStage&) = delete;
    ScopedStage& operator=(const ScopedStage&) = delete;

private:
    const char* previous;
};
} // namespace BbWatch
