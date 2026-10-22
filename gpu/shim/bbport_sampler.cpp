// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: sampling profiler for slow frames (see bbport_sampler.h).

#include "bbport_sampler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace BbSampler {
namespace {

using Clock = std::chrono::steady_clock;

constexpr unsigned MaxThreads = 4;
constexpr unsigned MaxDepth = 24;
constexpr unsigned RingSize = 8192; // per thread: ~4 s at 0.5 ms

struct Sample {
    Clock::time_point time;
    unsigned depth = 0;
    std::uint64_t frames[MaxDepth];
};

struct Thread {
    const char* name = nullptr;
#ifdef _WIN32
    HANDLE handle = nullptr;
#endif
    std::vector<Sample> ring;
    std::atomic<std::uint64_t> written{0};
};

struct Window {
    Clock::time_point start, end;
    double frame_ms;
};

std::array<Thread, MaxThreads> threads;
std::atomic<unsigned> thread_count{0};
std::mutex window_mutex;
std::vector<Window> windows;

bool ReadEnabled() {
    const char* env = std::getenv("BB_HITCH_PROFILE");
    return env && env[0] == '1';
}

#ifdef _WIN32
extern "C" char __ImageBase;

std::uint64_t ImageAddress(DWORD64 pc) {
    return pc - reinterpret_cast<std::uintptr_t>(&__ImageBase) + 0x140000000ull;
}

/// Unwinds a suspended thread's context; no allocation (the thread may hold the heap lock).
unsigned Unwind(CONTEXT context, std::uint64_t* frames) {
    unsigned depth = 0;
    while (depth < MaxDepth && context.Rip != 0) {
        frames[depth++] = context.Rip;
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (!function) {
            // Only a leaf function of this image at the top: its return address is at the top
            // of the stack. Anything else without unwind data (guest code) ends the walk: its
            // stack layout is unknown, and reading through it faulted.
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(&__ImageBase);
            if (depth != 1 || context.Rip < base || context.Rip >= base + 0x10000000ull ||
                context.Rsp == 0) {
                break;
            }
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                         &handler_data, &establisher, nullptr);
    }
    return depth;
}

void PrintWindow(const Window& window) {
    // Per thread: how many samples fell in the window, and per image address how many of those
    // samples had it on the stack (once per sample).
    for (unsigned t = 0; t < thread_count.load(std::memory_order_acquire); ++t) {
        Thread& thread = threads[t];
        const std::uint64_t written = thread.written.load(std::memory_order_acquire);
        const std::uint64_t first = written > RingSize ? written - RingSize : 0;
        std::unordered_map<std::uint64_t, unsigned> counts;
        std::unordered_map<std::uint64_t, unsigned> leaves;
        unsigned samples = 0;
        for (std::uint64_t i = first; i < written; ++i) {
            const Sample& sample = thread.ring[i % RingSize];
            if (sample.time < window.start || sample.time > window.end) {
                continue;
            }
            ++samples;
            std::uint64_t seen[MaxDepth];
            unsigned unique = 0;
            for (unsigned d = 0; d < sample.depth; ++d) {
                const std::uint64_t address = ImageAddress(sample.frames[d]);
                if (std::find(seen, seen + unique, address) != seen + unique) {
                    continue;
                }
                seen[unique++] = address;
                ++counts[address];
            }
            if (sample.depth) {
                ++leaves[ImageAddress(sample.frames[0])];
            }
        }
        if (samples == 0) {
            continue;
        }
        std::vector<std::pair<std::uint64_t, unsigned>> sorted(counts.begin(), counts.end());
        std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
        std::printf("Hitch profile: %.0f ms frame, thread %s, %u samples; on stack:", window.frame_ms,
                    thread.name, samples);
        for (size_t i = 0; i < std::min<size_t>(sorted.size(), 40); ++i) {
            std::printf(" 0x%llx=%u", static_cast<unsigned long long>(sorted[i].first),
                        sorted[i].second);
        }
        std::vector<std::pair<std::uint64_t, unsigned>> top(leaves.begin(), leaves.end());
        std::ranges::sort(top, [](const auto& a, const auto& b) { return a.second > b.second; });
        std::printf("; leaves:");
        for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
            std::printf(" 0x%llx=%u", static_cast<unsigned long long>(top[i].first), top[i].second);
        }
        std::printf("\n");
    }
}

void Run() {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (;;) {
        if (timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -5000; // 0.5 ms
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }
        const unsigned count = thread_count.load(std::memory_order_acquire);
        for (unsigned t = 0; t < count; ++t) {
            Thread& thread = threads[t];
            const std::uint64_t index = thread.written.load(std::memory_order_relaxed);
            Sample& sample = thread.ring[index % RingSize];
            if (SuspendThread(thread.handle) == DWORD(-1)) {
                continue;
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread.handle, &context)) {
                sample.depth = Unwind(context, sample.frames);
            } else {
                sample.depth = 0;
            }
            ResumeThread(thread.handle);
            sample.time = Clock::now();
            thread.written.store(index + 1, std::memory_order_release);
        }
        std::vector<Window> pending;
        {
            std::scoped_lock lock{window_mutex};
            // A window is printed once its end is a little in the past (its samples are in).
            auto ready = std::partition(windows.begin(), windows.end(), [](const Window& w) {
                return Clock::now() - w.end < std::chrono::milliseconds(5);
            });
            pending.assign(ready, windows.end());
            windows.erase(ready, windows.end());
        }
        for (const Window& window : pending) {
            PrintWindow(window);
        }
    }
}
#endif

} // namespace

bool Enabled() {
    static const bool enabled = ReadEnabled();
    return enabled;
}

void RegisterThread(const char* name) {
#ifdef _WIN32
    // Registered also without BB_HITCH_PROFILE, for DumpStacks; sampled only with it.
    thread_local bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    static std::mutex mutex;
    std::scoped_lock lock{mutex};
    const unsigned index = thread_count.load(std::memory_order_relaxed);
    if (index == MaxThreads) {
        return;
    }
    Thread& thread = threads[index];
    thread.name = name;
    if (Enabled()) {
        thread.ring.resize(RingSize);
    }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread.handle,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                    0);
    thread_count.store(index + 1, std::memory_order_release);
    if (!Enabled()) {
        return;
    }
    std::printf("Hitch profile: sampling thread %s\n", name);
    if (index == 0) {
        std::thread{Run}.detach();
    }
#else
    (void)name;
#endif
}

void DumpStacks() {
#ifdef _WIN32
    for (unsigned t = 0; t < thread_count.load(std::memory_order_acquire); ++t) {
        Thread& thread = threads[t];
        std::uint64_t frames[MaxDepth];
        unsigned depth = 0;
        if (SuspendThread(thread.handle) == DWORD(-1)) {
            continue;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread.handle, &context)) {
            depth = Unwind(context, frames);
        }
        ResumeThread(thread.handle);
        std::printf("Stack of %s:", thread.name);
        for (unsigned d = 0; d < depth; ++d) {
            // This image's frames as image addresses (llvm-symbolizer); others (the driver,
            // ntdll) as module+offset.
            HMODULE module = nullptr;
            char path[MAX_PATH] = "?";
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(frames[d]), &module);
            if (module == reinterpret_cast<HMODULE>(&__ImageBase)) {
                std::printf(" 0x%llx", static_cast<unsigned long long>(ImageAddress(frames[d])));
                continue;
            }
            if (module) {
                GetModuleFileNameA(module, path, MAX_PATH);
            }
            const char* slash = std::strrchr(path, '\\');
            std::printf(" %s+0x%llx", slash ? slash + 1 : path,
                        static_cast<unsigned long long>(
                            frames[d] - reinterpret_cast<std::uintptr_t>(module)));
        }
        std::printf("\n");
    }
    std::fflush(stdout);
#endif
}

void ReportWindow(Clock::time_point start, Clock::time_point end, double frame_ms) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lock{window_mutex};
    if (windows.size() < 16) {
        windows.push_back({start, end, frame_ms});
    }
}

} // namespace BbSampler
