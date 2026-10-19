// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>

// bbport: BB_HITCH_PROFILE=1 — a sampling profiler for slow frames (Windows). Threads that
// register are sampled every ~0.5 ms (suspended, their stack unwound through the image's unwind
// tables, resumed); for each slow frame VideoOut reports, the frames found most often in that
// frame's samples are printed as addresses in out/bb-probe.exe, for llvm-symbolizer
// (scripts or by hand: llvm-symbolizer --obj=out/bb-probe.exe 0x...).
namespace BbSampler {

/// Whether BB_HITCH_PROFILE=1.
bool Enabled();

/// The calling thread is sampled from now on (once per thread; `name` must outlive it).
void RegisterThread(const char* name);

/// A slow frame spanned [start, end]: its samples are printed by the sampler thread.
void ReportWindow(std::chrono::steady_clock::time_point start,
                  std::chrono::steady_clock::time_point end, double frame_ms);

} // namespace BbSampler
