// SPDX-License-Identifier: GPL-2.0-or-later
// The lamp travel menu (launcher: Quality of Life), drawn by the overlay. The game asks for it
// through event flags (src/runtime_lamp.c, C interface in bbgpu.h).
#pragma once

#include <SDL3/SDL.h>

struct ImFontAtlas;

namespace BbLampMenu {

/// Overlay Init: the menu's serif font (Georgia on Windows), when available.
void LoadFont(ImFontAtlas* atlas);
bool IsOpen();
/// Window thread, under the overlay's ImGui lock, for every SDL event while open: always true.
bool HandleEvent(const SDL_Event& event);
/// Present thread, between ImGui::NewFrame and ImGui::Render.
void Draw();

} // namespace BbLampMenu
