// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

// bbport: system commit charge, for diagnosing allocation failures. On Windows, host-visible
// Vulkan memory and VRAM that WDDM pages out both need commit (RAM plus page file), and the
// guest memory section alone commits ~6 GB at startup.
namespace BbHostMemory {
struct Commit {
    std::uint64_t system_used = 0;  ///< all processes
    std::uint64_t system_limit = 0; ///< RAM plus the current page file size
    std::uint64_t process = 0;      ///< this process's private commit
};
/// False where unavailable (non-Windows hosts).
bool GetCommit(Commit& commit);
} // namespace BbHostMemory
