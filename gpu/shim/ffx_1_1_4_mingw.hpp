// bbport: force-included into the FidelityFX 1.1.4 targets on MinGW (CLANG64).
// The SDK relies on MSVC including these transitively (_countof, swprintf_s,
// placement new, log2); FSR-Vulkan's own portability header is skipped on WIN32.
#pragma once

#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <new>

using std::floor;
using std::log2;
