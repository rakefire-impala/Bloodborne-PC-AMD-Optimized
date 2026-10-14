"""Run the production RegionManager through a CPU write into GPU-written pages and the upload.

An upload replaces whole pages with their guest copy, so afterwards those pages hold no GPU-written
data: their GPU bits clear (with readbacks Off nothing else ever cleared them). Page watchers are
counted per page and must never go below zero (the page manager asserts on that): only Precise
protects GPU pages for reads, so only Precise may drop read watchers.
"""
from pathlib import Path
import argparse
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default='g++')
args = parser.parse_args()
SOURCE = ROOT / 'gpu/shadps4'

STUBS = {
    'bbport_toggles.h': '''#pragma once
#include <atomic>
#include <cstdint>
namespace BbToggle {
enum : std::uint64_t { PageTrackingEarlyExit = 4, HotPages = 32768 };
inline bool Disabled(std::uint64_t) { return false; }
}
namespace BbStats {
inline std::atomic<std::uint64_t> tracker_faults{0};
inline std::atomic<std::int64_t> hot_pages{0};
}
''',
    'common/logging/log.h': '#pragma once\n',
    'common/debug.h': '#pragma once\n#define RENDERER_TRACE\n',
    'common/spin_lock.h': '''#pragma once
namespace Common { struct SpinLock { void lock() {} void unlock() {} }; }
''',
    'common/types.h': '''#pragma once
#include <cstdint>
#include <cstddef>
using u8 = std::uint8_t; using u16 = std::uint16_t; using u32 = std::uint32_t;
using u64 = std::uint64_t; using s32 = std::int32_t; using s64 = std::int64_t;
using VAddr = std::uintptr_t;
''',
    'core/emulator_settings.h': '''#pragma once
#include "common/types.h"
enum GpuReadbacksMode : int { Disabled, Relaxed, Precise };
struct TestSettings { u32 mode = 0; u32 GetReadbacksMode() const { return mode; } };
inline TestSettings EmulatorSettings;
''',
    'video_core/page_manager.h': '''#pragma once
#include <cstdio>
#include <map>
#include "video_core/buffer_cache/region_definitions.h"
namespace VideoCore {
/// Watchers per page as the real page manager counts them; underflow is what it asserts on.
class PageManager {
public:
    struct Watch { int write = 0, read = 0; };
    std::map<VAddr, Watch> pages;
    int underflows = 0;
    template <bool track, bool is_read = false>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) {
        for (const auto& [start, end] : mask) {
            for (size_t page = start; page < end; ++page) {
                Watch& watch = pages[base_addr + page * TRACKER_BYTES_PER_PAGE];
                int& count = is_read ? watch.read : watch.write;
                count += track ? 1 : -1;
                if (count < 0) {
                    ++underflows;
                    std::printf("watcher underflow: %s page %zu\\n", is_read ? "read" : "write", page);
                }
            }
        }
    }
};
}
''',
}

FIXTURE = r'''
#include <cassert>
#include <cstdio>
#include <vector>
#include "video_core/buffer_cache/region_manager.h"
using namespace VideoCore;

constexpr VAddr BASE = 0x10000000;
constexpr u64 PAGE = TRACKER_BYTES_PER_PAGE;

int main() {
    int failures = 0;
    for (u32 mode : {0u, 1u, 2u}) {
        EmulatorSettings.mode = mode;
        PageManager tracker;
        RegionManager region(&tracker, BASE);
        // Everything starts CPU-modified: the first upload protects every page for writes.
        region.ForEachModifiedRange<Type::CPU, true>(BASE, TRACKER_HIGHER_PAGE_SIZE,
                                                     [](VAddr, u64) {});
        // The GPU writes pages 0-3; the CPU then writes page 1 (a write fault).
        region.ChangeRegionState<Type::GPU, true>(BASE, 4 * PAGE);
        region.ChangeRegionState<Type::CPU, true>(BASE + PAGE, PAGE);

        std::vector<std::pair<VAddr, u64>> both, gpu;
        region.ForEachGpuModifiedRange<true>(0, 4 * PAGE,
                                             [&](VAddr a, u64 s) { both.emplace_back(a, s); });
        region.ForEachGpuModifiedRange<false>(0, 4 * PAGE,
                                              [&](VAddr a, u64 s) { gpu.emplace_back(a, s); });
        const bool audit_ok = both.size() == 1 && both[0] == std::pair<VAddr, u64>{BASE + PAGE, PAGE} &&
                              gpu.size() == 1 && gpu[0] == std::pair<VAddr, u64>{BASE, 4 * PAGE};

        std::vector<std::pair<VAddr, u64>> uploads;
        region.ForEachModifiedRange<Type::CPU, true>(BASE, 4 * PAGE,
                                                     [&](VAddr a, u64 s) { uploads.emplace_back(a, s); });
        const bool upload_ok = uploads.size() == 1 && uploads[0] == std::pair<VAddr, u64>{BASE + PAGE, PAGE};
        // Uploaded whole: page 1 has no GPU data left; the others keep theirs.
        const bool bits_ok = region.IsRegionModified<Type::GPU>(0, PAGE) &&
                             !region.IsRegionModified<Type::GPU>(PAGE, PAGE) &&
                             region.IsRegionModified<Type::GPU>(2 * PAGE, 2 * PAGE) &&
                             !region.IsRegionModified<Type::CPU>(0, 4 * PAGE);
        bool watch_ok = tracker.underflows == 0;
        for (u64 page = 0; page < 4; ++page) {
            const auto& watch = tracker.pages[BASE + page * PAGE];
            const int read = mode == 2 && page != 1 ? 1 : 0;
            watch_ok = watch_ok && watch.write == 1 && watch.read == read;
        }
        std::printf("readbacks %s: audit ranges %s, upload %s, GPU bits %s, watchers %s\n",
                    mode == 0 ? "Off" : mode == 1 ? "Relaxed" : "Precise", audit_ok ? "ok" : "WRONG",
                    upload_ok ? "ok" : "WRONG", bits_ok ? "ok" : "WRONG", watch_ok ? "ok" : "WRONG");
        failures += !audit_ok + !upload_ok + !bits_ok + !watch_ok;
    }
    return failures ? 1 : 0;
}
'''

with tempfile.TemporaryDirectory(prefix='bb-region-upload-') as directory:
    temp = Path(directory)
    for name, text in STUBS.items():
        (temp / name).parent.mkdir(parents=True, exist_ok=True)
        (temp / name).write_text(text)
    for name in ('common/bit_array.h', 'common/div_ceil.h',
                 'video_core/buffer_cache/region_definitions.h',
                 'video_core/buffer_cache/region_manager.h'):
        (temp / name).parent.mkdir(parents=True, exist_ok=True)
        (temp / name).write_text((SOURCE / name).read_text())
    (temp / 'fixture.cpp').write_text(FIXTURE)
    subprocess.run(shlex.split(args.compiler) + ['-std=c++23', '-O2', '-Wall', '-Wextra', '-Werror',
                                                 '-Wno-unused-parameter', '-Wno-sign-compare',
                                                 '-I' + str(temp),
                                                 str(temp / 'fixture.cpp'), '-o', str(temp / 'fixture')],
                   check=True)
    result = subprocess.run([str(temp / 'fixture')], capture_output=True, text=True, timeout=60)
    print(result.stdout.strip())
    assert result.returncode == 0, result.stderr
print('PASS: whole-page uploads clear GPU bits without unbalancing page watchers in any readback mode.')
