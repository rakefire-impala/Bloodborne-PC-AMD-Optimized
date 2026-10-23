// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_splash.h"

#include <atomic>
#include <chrono>
#include <cstdio>

#ifdef _WIN32
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <wincodec.h>
#endif

namespace BbSplash {
namespace {

std::atomic<bool> presented{false};

#ifdef _WIN32
using Clock = std::chrono::steady_clock;

struct Image {
    std::vector<unsigned char> pixels; ///< 32-bit BGRX rows, top-down
    int width = 0, height = 0;
};

std::mutex image_mutex;
Image image;          ///< decoded, guarded by image_mutex until the window thread takes it
bool decoded = false; ///< image_mutex
HWND window{};
Image shown;          ///< window thread only
RECT painted_rect{};
Clock::time_point started, last_paint;
bool active = false, ended = false;

/// The image scaled to cover `width` x `height` (centred, cropped by the paint), with WIC
/// (Windows' own decoders: progressive JPEG, PNG). Empty on any failure.
Image Decode(const std::wstring& path, int width, int height) {
    Image result;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        return result;
    }
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* converter = nullptr;
    UINT source_w = 0, source_h = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&source_w, &source_h)) &&
        source_w && source_h) {
        const double scale = std::max(double(width) / source_w, double(height) / source_h);
        const UINT w = std::max(1u, UINT(std::lround(source_w * scale)));
        const UINT h = std::max(1u, UINT(std::lround(source_h * scale)));
        if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame, w, h, WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(scaler, GUID_WICPixelFormat32bppBGR,
                                            WICBitmapDitherTypeNone, nullptr, 0,
                                            WICBitmapPaletteTypeCustom))) {
            result.pixels.resize(size_t(w) * h * 4);
            if (SUCCEEDED(converter->CopyPixels(nullptr, w * 4, UINT(result.pixels.size()),
                                                result.pixels.data()))) {
                result.width = int(w);
                result.height = int(h);
            } else {
                result.pixels.clear();
            }
        }
    }
    for (IUnknown* object : {static_cast<IUnknown*>(converter), static_cast<IUnknown*>(scaler),
                             static_cast<IUnknown*>(frame), static_cast<IUnknown*>(decoder),
                             static_cast<IUnknown*>(factory)}) {
        if (object) object->Release();
    }
    CoUninitialize();
    return result;
}

void PaintImage() {
    RECT client{};
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) {
        return; // minimized
    }
    HDC dc = GetDC(window);
    if (!dc) return;
    // Cover the client area, centred: the decoded size fits the window at decode time; a
    // window resized since (fullscreen switch) is covered by stretching.
    const double scale = std::max(double(client.right) / shown.width,
                                  double(client.bottom) / shown.height);
    const int w = int(std::lround(shown.width * scale)), h = int(std::lround(shown.height * scale));
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = shown.width;
    info.bmiHeader.biHeight = -shown.height; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, (client.right - w) / 2, (client.bottom - h) / 2, w, h, 0, 0, shown.width,
                  shown.height, shown.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(window, dc);
    painted_rect = client;
}
#endif

} // namespace

void Start(void* hwnd) {
#ifdef _WIN32
    const wchar_t* path = _wgetenv(L"BB_SPLASH");
    if (!hwnd || !path || !*path || presented.load()) {
        return;
    }
    window = static_cast<HWND>(hwnd);
    RECT client{};
    GetClientRect(window, &client);
    const int width = std::max<int>(client.right, 1), height = std::max<int>(client.bottom, 1);
    started = Clock::now();
    active = true;
    std::thread([file = std::wstring{path}, width, height] {
        auto result = Decode(file, width, height);
        if (result.pixels.empty()) {
            std::printf("Splash: the image could not be read (BB_SPLASH)\n");
        }
        std::scoped_lock lock{image_mutex};
        image = std::move(result);
        decoded = true;
    }).detach();
#else
    (void)hwnd;
#endif
}

void Paint() {
#ifdef _WIN32
    if (!active || ended) {
        return;
    }
    if (presented.load(std::memory_order_relaxed)) {
        ended = true;
        shown = {};
        const double seconds =
            std::chrono::duration<double>(Clock::now() - started).count();
        std::printf("Splash: shown until the first frame (%.1f s)\n", seconds);
        return;
    }
    if (shown.pixels.empty()) {
        std::scoped_lock lock{image_mutex};
        if (!decoded) return;
        shown = std::move(image);
        if (shown.pixels.empty()) {
            ended = true;
            return;
        }
        last_paint = {};
    }
    // Repainted twice a second (and at once after a resize): Windows may have drawn over it.
    RECT client{};
    GetClientRect(window, &client);
    const auto now = Clock::now();
    if (now - last_paint >= std::chrono::milliseconds(500) || client.right != painted_rect.right ||
        client.bottom != painted_rect.bottom) {
        PaintImage();
        last_paint = now;
    }
#endif
}

void OnFramePresented() {
    presented.store(true, std::memory_order_relaxed);
}

} // namespace BbSplash
