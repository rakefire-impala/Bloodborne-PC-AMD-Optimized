// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_host_memory.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#undef near
#endif

namespace BbHostMemory {
bool GetCommit(Commit& commit) {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return false;
    }
    commit.system_limit = status.ullTotalPageFile;
    commit.system_used = status.ullTotalPageFile - status.ullAvailPageFile;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters))) {
        commit.process = counters.PrivateUsage;
    }
    return true;
#else
    (void)commit;
    return false;
#endif
}
} // namespace BbHostMemory
