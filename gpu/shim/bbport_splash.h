// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the startup splash. The image (BB_SPLASH; run.py passes bb.jpg from the port folder)
// fills the game window from its creation until the renderer presents its first frame, instead
// of an empty window during the shader warm-up and the game's boot.
#pragma once

namespace BbSplash {

/// Window thread, once the window exists: the image is decoded on a helper thread.
void Start(void* hwnd);
/// Window thread, every event-loop pass: paints the image (throttled) until the first frame.
void Paint();
/// Renderer: a frame was presented; the splash ends.
void OnFramePresented();

} // namespace BbSplash
