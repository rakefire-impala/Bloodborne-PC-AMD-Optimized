// SPDX-License-Identifier: GPL-2.0-or-later
// The lamp travel menu: a tab per headstone of the Hunter's Dream (L1/R1, only headstones with a
// lit lamp), its lamps in the headstone's order ("???" until lit, the lamp the player stands at
// marked), and the Hunter's Dream on Square. Texts are the game's own, in the game's language
// (scripts/lamp_travel.py writes them to lamp_menu.txt).
//
// The boss retry prompt (src/runtime_retry.c) shares its look: the boss's name, a question, and
// Cross to retry or Circle to decline.
#include "bbport_lamp_menu.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "../bbgpu.h"
#include "imgui.h"

namespace BbLampMenu {

namespace {

struct Lamp {
    int flag = 0, lit_flag = 0;
    std::string name;
};
struct Tab {
    std::string name;
    std::vector<int> lamps;
};
enum class Device { Keyboard, PlayStation, Xbox };
enum class Button { Select, Back, Dream, Previous, Next };

// Loaded once (runtime install), read-only afterwards.
std::vector<Lamp> lamps;
std::vector<Tab> tabs;
std::vector<int> menu_flags, lit_flags;
std::string dream_name, select_text, return_text;
ImFont* serif = nullptr;

// The open menu: game thread (open/close/result), window thread (input), present thread (draw).
std::mutex mutex;
std::atomic<bool> shown{false};
bool lit[BBGPU_LAMP_MENU_MAX] = {};
int here = -1, tab = 0, cursor = -1;
int result = BBGPU_LAMP_PENDING;
Device device = Device::Keyboard;
bool detect_device = false;
bool stick_x_held = false, stick_y_held = false;

// The open retry prompt, under the same mutex.
std::atomic<bool> prompt_shown{false};
std::string prompt_boss, prompt_question, prompt_retry, prompt_decline;
int prompt_result = BBGPU_RETRY_PENDING;

bool TabVisible(int t) {
    for (int i : tabs[t].lamps) {
        if (lit[i]) return true;
    }
    return false;
}

bool Selectable(int i) {
    return lit[i] && i != here;
}

void FirstSelectable() {
    cursor = -1;
    for (int i : tabs[tab].lamps) {
        if (Selectable(i)) {
            cursor = i;
            return;
        }
    }
}

void MoveCursor(int direction) {
    const auto& list = tabs[tab].lamps;
    const int n = int(list.size());
    int at = int(std::find(list.begin(), list.end(), cursor) - list.begin());
    if (at >= n) at = direction > 0 ? -1 : n;
    for (int step = 1; step <= n; ++step) {
        const int candidate = list[((at + direction * step) % n + n) % n];
        if (Selectable(candidate)) {
            cursor = candidate;
            return;
        }
    }
}

void MoveTab(int direction) {
    const int n = int(tabs.size());
    for (int step = 1; step < n; ++step) {
        const int candidate = ((tab + direction * step) % n + n) % n;
        if (TabVisible(candidate)) {
            tab = candidate;
            FirstSelectable();
            return;
        }
    }
}

int VisibleTabs() {
    int n = 0;
    for (int t = 0; t < int(tabs.size()); ++t) n += TabVisible(t);
    return n;
}

void Finish(int value) {
    result = value;
    shown = false;
}

void Press(Button button) {
    switch (button) {
    case Button::Select:
        if (cursor >= 0) Finish(cursor);
        break;
    case Button::Back: Finish(BBGPU_LAMP_CANCEL); break;
    case Button::Dream: Finish(BBGPU_LAMP_DREAM); break;
    case Button::Previous: MoveTab(-1); break;
    case Button::Next: MoveTab(1); break;
    }
}

Device DeviceOf(SDL_JoystickID id) {
    switch (SDL_GetGamepadTypeForID(id)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return Device::PlayStation;
    default: return Device::Xbox;
    }
}

// --- drawing (1080p units, scaled) ---

constexpr ImU32 TextColor = IM_COL32(196, 188, 172, 255);
constexpr ImU32 BrightColor = IM_COL32(250, 236, 196, 255);
constexpr ImU32 DimColor = IM_COL32(118, 114, 106, 255);
constexpr ImU32 HereColor = IM_COL32(214, 178, 112, 255);
constexpr ImU32 LineColor = IM_COL32(150, 142, 124, 210);

ImFont* Font() {
    return serif ? serif : ImGui::GetFont();
}

ImVec2 TextSize(float size, const char* text) {
    return Font()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

void Text(ImDrawList* d, float size, ImVec2 pos, ImU32 color, const char* text) {
    d->AddText(Font(), size, ImVec2(pos.x + size * 0.04f, pos.y + size * 0.05f), IM_COL32(0, 0, 0, 200), text);
    d->AddText(Font(), size, pos, color, text);
}

// A thin ornamental rule: a line with a diamond in the middle and dots at the ends.
void Rule(ImDrawList* d, float x0, float x1, float y, float s) {
    const float mid = (x0 + x1) * 0.5f;
    d->AddLine(ImVec2(x0, y), ImVec2(x1, y), LineColor, std::max(1.0f, 1.2f * s));
    d->AddLine(ImVec2(mid - 90 * s, y - 3 * s), ImVec2(mid + 90 * s, y - 3 * s), IM_COL32(150, 142, 124, 120),
               std::max(1.0f, s));
    d->AddQuadFilled(ImVec2(mid, y - 7 * s), ImVec2(mid + 7 * s, y), ImVec2(mid, y + 7 * s), ImVec2(mid - 7 * s, y),
                     IM_COL32(176, 166, 144, 255));
    d->AddCircleFilled(ImVec2(x0, y), 2.5f * s, LineColor);
    d->AddCircleFilled(ImVec2(x1, y), 2.5f * s, LineColor);
}

// Controller glyphs: PlayStation symbols, Xbox letters, or a keyboard key.
void Glyph(ImDrawList* d, ImVec2 c, float s, Button button) {
    const float r = 13.0f * s;
    if (device == Device::Keyboard || button == Button::Previous || button == Button::Next) {
        const char* label = "";
        switch (button) {
        case Button::Select: label = device == Device::Keyboard ? "Enter" : ""; break;
        case Button::Back: label = "Esc"; break;
        case Button::Dream: label = "E"; break;
        case Button::Previous: label = device == Device::Keyboard ? "1" : device == Device::PlayStation ? "L1" : "LB"; break;
        case Button::Next: label = device == Device::Keyboard ? "3" : device == Device::PlayStation ? "R1" : "RB"; break;
        }
        const float size = 18.0f * s;
        const ImVec2 t = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
        const float w = std::max(t.x + 14.0f * s, 2.0f * r);
        d->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - r), ImVec2(c.x + w * 0.5f, c.y + r), IM_COL32(20, 20, 22, 230), 4 * s);
        d->AddRect(ImVec2(c.x - w * 0.5f, c.y - r), ImVec2(c.x + w * 0.5f, c.y + r), IM_COL32(210, 204, 190, 255), 4 * s, 0,
                   std::max(1.0f, 1.3f * s));
        d->AddText(ImGui::GetFont(), size, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), IM_COL32(236, 232, 222, 255), label);
        return;
    }
    d->AddCircleFilled(c, r, IM_COL32(18, 20, 28, 240));
    d->AddCircle(c, r, IM_COL32(150, 150, 156, 255), 0, std::max(1.0f, 1.2f * s));
    if (device == Device::PlayStation) {
        const float k = r * 0.48f, w = std::max(1.5f, 2.4f * s);
        switch (button) {
        case Button::Select: {
            const ImU32 blue = IM_COL32(120, 150, 240, 255);
            d->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), blue, w);
            d->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), blue, w);
            break;
        }
        case Button::Back: d->AddCircle(c, k * 1.05f, IM_COL32(236, 80, 80, 255), 0, w); break;
        case Button::Dream:
            d->AddRect(ImVec2(c.x - k * 0.9f, c.y - k * 0.9f), ImVec2(c.x + k * 0.9f, c.y + k * 0.9f),
                       IM_COL32(232, 130, 200, 255), 0.0f, 0, w);
            break;
        default: break;
        }
        return;
    }
    const char* letter = button == Button::Select ? "A" : button == Button::Back ? "B" : "X";
    const ImU32 color = button == Button::Select ? IM_COL32(110, 200, 90, 255)
                      : button == Button::Back   ? IM_COL32(236, 80, 80, 255)
                                                 : IM_COL32(90, 140, 240, 255);
    const float size = 19.0f * s;
    const ImVec2 t = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, letter);
    d->AddText(ImGui::GetFont(), size, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), color, letter);
}

// A headstone (rounded top) beside each lamp; lit gold under the cursor.
void Stone(ImDrawList* d, ImVec2 c, float s, bool selected, bool current) {
    const ImVec2 a(c.x - 9 * s, c.y - 13 * s), b(c.x + 9 * s, c.y + 13 * s);
    if (selected) d->AddCircleFilled(c, 22 * s, IM_COL32(230, 180, 90, 50));
    const ImU32 fill = selected ? IM_COL32(222, 184, 110, 255) : current ? IM_COL32(150, 126, 84, 255)
                                                                         : IM_COL32(112, 112, 112, 255);
    d->AddRectFilled(a, b, fill, 8 * s, ImDrawFlags_RoundCornersTop);
    d->AddRect(a, b, IM_COL32(40, 38, 34, 255), 8 * s, ImDrawFlags_RoundCornersTop, std::max(1.0f, s));
    d->AddLine(ImVec2(c.x - 4 * s, c.y - 4 * s), ImVec2(c.x + 4 * s, c.y - 4 * s), IM_COL32(40, 38, 34, 200),
               std::max(1.0f, s));
    if (current) d->AddCircleFilled(ImVec2(c.x, c.y + 4 * s), 3.5f * s, IM_COL32(255, 210, 120, 255));
}

bool Hovered(ImVec2 a, ImVec2 b) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    return m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
}

// --- the retry prompt ---

void Answer(int value) {
    prompt_result = value;
    prompt_shown = false;
}

bool PromptEvent(const SDL_Event& event) {
    ImGuiIO& io = ImGui::GetIO();
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        device = DeviceOf(event.gbutton.which);
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) Answer(1);
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) Answer(0);
        return true;
    case SDL_EVENT_KEY_DOWN:
        device = Device::Keyboard;
        if (event.key.repeat) return true;
        switch (event.key.key) {
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: Answer(1); break;
        case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_LSHIFT: Answer(0); break;
        default: break;
        }
        return true;
    case SDL_EVENT_MOUSE_MOTION: {
        SDL_Window* window = SDL_GetWindowFromID(event.motion.windowID);
        const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        SDL_Window* window = SDL_GetWindowFromID(event.button.windowID);
        const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
        io.AddMousePosEvent(event.button.x * density, event.button.y * density);
        if (event.button.button == SDL_BUTTON_LEFT) io.AddMouseButtonEvent(0, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_TEXT_INPUT:
        return true;
    default:
        return false;
    }
}

void PromptDraw() {
    if (detect_device) {
        detect_device = false;
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        device = ids && count > 0 ? DeviceOf(ids[0]) : Device::Keyboard;
        SDL_free(ids);
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 screen = viewport->Size, origin = viewport->Pos;
    const float s = std::max(0.5f, std::min(screen.y / 1080.0f, screen.x / 1920.0f));
    ImDrawList* d = ImGui::GetForegroundDrawList();
    const float panel_w = std::min(900.0f * s, screen.x - 32.0f * s);
    const float x0 = origin.x + (screen.x - panel_w) * 0.5f, x1 = x0 + panel_w;
    const float top = origin.y + screen.y * 0.5f - 130.0f * s, bottom = top + 230.0f * s;

    // The lamp menu's band, lower and without the dimming: the world stays visible behind it.
    const float fade = 120.0f * s;
    const ImU32 band = IM_COL32(4, 4, 6, 205), clear = IM_COL32(4, 4, 6, 0);
    d->AddRectFilled(ImVec2(x0 + fade, top - 20 * s), ImVec2(x1 - fade, bottom + 20 * s), band);
    d->AddRectFilledMultiColor(ImVec2(x0, top - 20 * s), ImVec2(x0 + fade, bottom + 20 * s), clear, band, band, clear);
    d->AddRectFilledMultiColor(ImVec2(x1 - fade, top - 20 * s), ImVec2(x1, bottom + 20 * s), band, clear, clear, band);
    Rule(d, x0 + 40 * s, x1 - 40 * s, top, s);
    Rule(d, x0 + 40 * s, x1 - 40 * s, bottom, s);

    const float title_size = 34.0f * s, question_size = 27.0f * s;
    const ImVec2 title = TextSize(title_size, prompt_boss.c_str());
    Text(d, title_size, ImVec2((x0 + x1 - title.x) * 0.5f, top + 28 * s), BrightColor, prompt_boss.c_str());
    const ImVec2 asked = TextSize(question_size, prompt_question.c_str());
    Text(d, question_size, ImVec2((x0 + x1 - asked.x) * 0.5f, top + 88 * s), TextColor, prompt_question.c_str());
    d->AddLine(ImVec2(x0 + 60 * s, top + 140 * s), ImVec2(x1 - 60 * s, top + 140 * s), IM_COL32(150, 142, 124, 120),
               std::max(1.0f, s));

    // The two choices, each clickable.
    const float key_size = 25.0f * s, gap = 64.0f * s, glyph = 34.0f * s;
    const float retry_w = glyph + TextSize(key_size, prompt_retry.c_str()).x;
    const float decline_w = glyph + TextSize(key_size, prompt_decline.c_str()).x;
    float kx = (x0 + x1 - retry_w - gap - decline_w) * 0.5f;
    const float ky = top + 185.0f * s;
    const bool clicked = ImGui::IsMouseClicked(0);
    for (int choice = 1; choice >= 0; --choice) {
        const std::string& label = choice ? prompt_retry : prompt_decline;
        const float w = choice ? retry_w : decline_w;
        const ImVec2 a(kx - 8 * s, ky - 22 * s), b(kx + w + 8 * s, ky + 22 * s);
        const bool hover = Hovered(a, b);
        if (clicked && hover) {
            Answer(choice);
            return;
        }
        Glyph(d, ImVec2(kx + 14 * s, ky), s, choice ? Button::Select : Button::Back);
        Text(d, key_size, ImVec2(kx + glyph, ky - key_size * 0.6f), hover ? BrightColor : TextColor, label.c_str());
        kx += w + gap;
    }
}

} // namespace

void LoadFont(ImFontAtlas* atlas) {
#ifdef _WIN32
    const char* windows = std::getenv("WINDIR");
    const std::string path = std::string(windows ? windows : "C:\\Windows") + "\\Fonts\\georgia.ttf";
#else
    const std::string path = "/usr/share/fonts/truetype/msttcorefonts/Georgia.ttf";
#endif
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
        serif = atlas->AddFontFromFileTTF(path.c_str(), 26.0f);
    }
}

bool IsOpen() {
    return shown || prompt_shown;
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{mutex};
    if (prompt_shown) return PromptEvent(event);
    if (!shown) return false;
    ImGuiIO& io = ImGui::GetIO();
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        device = DeviceOf(event.gbutton.which);
        switch (event.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_SOUTH: Press(Button::Select); break;
        case SDL_GAMEPAD_BUTTON_EAST: Press(Button::Back); break;
        case SDL_GAMEPAD_BUTTON_WEST: Press(Button::Dream); break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: Press(Button::Previous); break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: Press(Button::Next); break;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: MoveCursor(-1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: MoveCursor(1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: Press(Button::Previous); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: Press(Button::Next); break;
        default: break;
        }
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const float v = event.gaxis.value / 32767.0f;
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            if (!stick_y_held && std::abs(v) > 0.6f) MoveCursor(v > 0 ? 1 : -1);
            stick_y_held = std::abs(v) > 0.3f && (stick_y_held || std::abs(v) > 0.6f);
        } else if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            if (!stick_x_held && std::abs(v) > 0.6f) MoveTab(v > 0 ? 1 : -1);
            stick_x_held = std::abs(v) > 0.3f && (stick_x_held || std::abs(v) > 0.6f);
        }
        return true;
    }
    case SDL_EVENT_KEY_DOWN:
        device = Device::Keyboard;
        switch (event.key.key) {
        case SDLK_UP: case SDLK_W: case SDLK_I: MoveCursor(-1); break;
        case SDLK_DOWN: case SDLK_S: case SDLK_K: MoveCursor(1); break;
        case SDLK_LEFT: case SDLK_A: case SDLK_J: case SDLK_1: if (!event.key.repeat) Press(Button::Previous); break;
        case SDLK_RIGHT: case SDLK_D: case SDLK_L: case SDLK_3: if (!event.key.repeat) Press(Button::Next); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: if (!event.key.repeat) Press(Button::Select); break;
        case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_LSHIFT: if (!event.key.repeat) Press(Button::Back); break;
        case SDLK_E: case SDLK_H: if (!event.key.repeat) Press(Button::Dream); break;
        default: break;
        }
        return true;
    case SDL_EVENT_MOUSE_MOTION: {
        SDL_Window* window = SDL_GetWindowFromID(event.motion.windowID);
        const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        SDL_Window* window = SDL_GetWindowFromID(event.button.windowID);
        const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
        io.AddMousePosEvent(event.button.x * density, event.button.y * density);
        const int button = event.button.button == SDL_BUTTON_LEFT ? 0 : event.button.button == SDL_BUTTON_RIGHT ? 1 : -1;
        if (button >= 0) io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (event.wheel.y != 0.0f) MoveCursor(event.wheel.y > 0 ? -1 : 1);
        return true;
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_TEXT_INPUT:
        return true;
    default:
        return false;
    }
}

void Draw() {
    std::scoped_lock lock{mutex};
    if (prompt_shown) {
        PromptDraw();
        return;
    }
    if (!shown || tabs.empty()) return;
    if (detect_device) {
        detect_device = false;
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        device = ids && count > 0 ? DeviceOf(ids[0]) : Device::Keyboard;
        SDL_free(ids);
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 screen = viewport->Size, origin = viewport->Pos;
    const float s = std::max(0.5f, std::min(screen.y / 1080.0f, screen.x / 1920.0f));
    ImDrawList* d = ImGui::GetForegroundDrawList();
    const bool clicked = ImGui::IsMouseClicked(0);
    if (ImGui::IsMouseClicked(1)) {
        Finish(BBGPU_LAMP_CANCEL);
        return;
    }

    int rows = 0;
    for (const Tab& t : tabs) rows = std::max(rows, int(t.lamps.size()));
    const float panel_w = std::min(1000.0f * s, screen.x - 32.0f * s);
    const float row_h = 52.0f * s;
    const float x0 = origin.x + (screen.x - panel_w) * 0.5f, x1 = x0 + panel_w;
    const float list_h = rows * row_h;
    const float total = 105.0f * s + list_h + 125.0f * s;
    const float top = origin.y + std::max(24.0f * s, (screen.y - total) * 0.5f);
    const float list_y = top + 105.0f * s;
    const float foot = list_y + list_h + 20.0f * s;
    const float bottom = foot + 85.0f * s;

    // Dim the game, then a dark band that fades out at both sides.
    d->AddRectFilled(origin, ImVec2(origin.x + screen.x, origin.y + screen.y), IM_COL32(0, 0, 0, 90));
    const float fade = 120.0f * s;
    const ImU32 band = IM_COL32(4, 4, 6, 205), clear = IM_COL32(4, 4, 6, 0);
    d->AddRectFilled(ImVec2(x0 + fade, top - 25 * s), ImVec2(x1 - fade, bottom + 25 * s), band);
    d->AddRectFilledMultiColor(ImVec2(x0, top - 25 * s), ImVec2(x0 + fade, bottom + 25 * s), clear, band, band, clear);
    d->AddRectFilledMultiColor(ImVec2(x1 - fade, top - 25 * s), ImVec2(x1, bottom + 25 * s), band, clear, clear, band);
    Rule(d, x0 + 40 * s, x1 - 40 * s, top, s);
    Rule(d, x0 + 40 * s, x1 - 40 * s, bottom, s);

    // Title: the headstone, with the tab buttons when there is more than one.
    const Tab& current = tabs[tab];
    const float title_size = 34.0f * s;
    const ImVec2 title = TextSize(title_size, current.name.c_str());
    const float title_y = top + 45.0f * s;
    Text(d, title_size, ImVec2((x0 + x1 - title.x) * 0.5f, title_y - title.y * 0.5f), BrightColor, current.name.c_str());
    d->AddLine(ImVec2(x0 + 60 * s, top + 85 * s), ImVec2(x1 - 60 * s, top + 85 * s), IM_COL32(150, 142, 124, 120),
               std::max(1.0f, s));
    if (VisibleTabs() > 1) {
        for (int side = 0; side < 2; ++side) {
            const float dir = side ? 1.0f : -1.0f;
            const float cx = side ? x1 - 95 * s : x0 + 95 * s;
            const ImVec2 tip(cx + dir * 42 * s, title_y);
            d->AddTriangleFilled(ImVec2(tip.x, tip.y), ImVec2(tip.x - dir * 12 * s, tip.y - 12 * s),
                                 ImVec2(tip.x - dir * 12 * s, tip.y + 12 * s), IM_COL32(210, 204, 190, 255));
            Glyph(d, ImVec2(cx, title_y), s, side ? Button::Next : Button::Previous);
            if (clicked && Hovered(ImVec2(cx - 40 * s, title_y - 22 * s), ImVec2(cx + 60 * s, title_y + 22 * s))) {
                Press(side ? Button::Next : Button::Previous);
                return;
            }
        }
    }

    // The lamps.
    const float text_size = 28.0f * s;
    const float row_x0 = x0 + 125 * s, row_x1 = x0 + panel_w * 0.64f;
    for (int r = 0; r < int(current.lamps.size()); ++r) {
        const int i = current.lamps[r];
        const float y = list_y + r * row_h;
        const ImVec2 a(row_x0, y), b(row_x1, y + row_h);
        if (Selectable(i) && Hovered(a, b)) {
            if (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f) cursor = i;
            if (clicked) {
                Finish(i);
                return;
            }
        }
        const bool selected = i == cursor;
        if (selected) {
            d->AddRectFilledMultiColor(a, b, IM_COL32(150, 110, 40, 110), IM_COL32(150, 110, 40, 0),
                                       IM_COL32(150, 110, 40, 0), IM_COL32(150, 110, 40, 110));
            d->AddRectFilledMultiColor(ImVec2(a.x, b.y - 2 * s), ImVec2(b.x, b.y), IM_COL32(236, 196, 120, 255),
                                       IM_COL32(236, 196, 120, 0), IM_COL32(236, 196, 120, 0), IM_COL32(236, 196, 120, 255));
        } else {
            d->AddLine(ImVec2(a.x + 30 * s, b.y), ImVec2(b.x, b.y), IM_COL32(90, 86, 78, 140), std::max(1.0f, s));
        }
        Stone(d, ImVec2(a.x + 28 * s, y + row_h * 0.5f), s, selected, i == here);
        const char* name = lit[i] ? lamps[i].name.c_str() : "???";
        const ImU32 color = !lit[i] ? DimColor : selected ? BrightColor : i == here ? HereColor : TextColor;
        const ImVec2 size = TextSize(text_size, name);
        Text(d, text_size, ImVec2(a.x + 62 * s, y + (row_h - size.y) * 0.5f), color, name);
    }

    // The Hunter's Dream, beside the list.
    const float dream_size = 26.0f * s;
    const ImVec2 dream_text = TextSize(dream_size, dream_name.c_str());
    const float box_w = std::max(300.0f * s, dream_text.x + 100.0f * s), box_h = 58.0f * s;
    float bx = x1 - 20 * s;
    if (bx + box_w > origin.x + screen.x - 16 * s) bx = std::max(row_x1 + 16 * s, origin.x + screen.x - 16 * s - box_w);
    const ImVec2 ba(bx, list_y), bb(bx + box_w, list_y + box_h);
    const bool dream_hover = Hovered(ba, bb);
    if (clicked && dream_hover) {
        Finish(BBGPU_LAMP_DREAM);
        return;
    }
    d->AddRectFilled(ba, bb, IM_COL32(6, 6, 8, dream_hover ? 240 : 215));
    d->AddRect(ba, bb, dream_hover ? IM_COL32(236, 196, 120, 255) : IM_COL32(150, 142, 124, 230), 0.0f, 0,
               std::max(1.0f, 1.2f * s));
    const float tick = 9 * s;
    for (const ImVec2& corner : {ba, ImVec2(bb.x, ba.y), bb, ImVec2(ba.x, bb.y)}) {
        d->AddCircleFilled(corner, 2.5f * s, IM_COL32(196, 186, 160, 255));
        d->AddLine(ImVec2(corner.x - tick, corner.y), ImVec2(corner.x + tick, corner.y), IM_COL32(196, 186, 160, 255),
                   std::max(1.0f, s));
    }
    Glyph(d, ImVec2(ba.x + 34 * s, list_y + box_h * 0.5f), s, Button::Dream);
    Text(d, dream_size, ImVec2(ba.x + 62 * s, list_y + (box_h - dream_text.y) * 0.5f), TextColor, dream_name.c_str());

    // Key guide.
    d->AddLine(ImVec2(x0 + 60 * s, foot), ImVec2(x1 - 60 * s, foot), IM_COL32(150, 142, 124, 120), std::max(1.0f, s));
    const float key_size = 25.0f * s;
    const std::string select = ": " + select_text, back = ": " + return_text;
    const float gap = 48.0f * s, glyph = 34.0f * s;
    const float select_w = glyph + TextSize(key_size, select.c_str()).x;
    const float width = select_w + gap + glyph + TextSize(key_size, back.c_str()).x;
    float kx = (x0 + x1 - width) * 0.5f;
    const float ky = foot + 34.0f * s;
    Glyph(d, ImVec2(kx + 14 * s, ky), s, Button::Select);
    Text(d, key_size, ImVec2(kx + glyph, ky - key_size * 0.6f), TextColor, select.c_str());
    kx += select_w + gap;
    Glyph(d, ImVec2(kx + 14 * s, ky), s, Button::Back);
    Text(d, key_size, ImVec2(kx + glyph, ky - key_size * 0.6f), TextColor, back.c_str());
}

} // namespace BbLampMenu

using namespace BbLampMenu;

extern "C" int bbgpu_lamp_menu_load(const char* path, int* request, int* done, int* dream,
                                    const int** lamp_flags, const int** lamp_lit_flags) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return 0;
    std::string line;
    int flags[3] = {};
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        std::stringstream fields(line);
        for (std::string field; std::getline(fields, field, '\t');) f.push_back(field);
        if (f.empty()) continue;
        if (f[0] == "dream" && f.size() >= 2) {
            dream_name = f[1];
        } else if (f[0] == "keys" && f.size() >= 3) {
            select_text = f[1];
            return_text = f[2];
        } else if (f[0] == "headstone" && f.size() >= 2) {
            tabs.push_back({f[1], {}});
        } else if (f[0] == "lamp" && f.size() >= 4 && !tabs.empty() && lamps.size() < BBGPU_LAMP_MENU_MAX) {
            tabs.back().lamps.push_back(int(lamps.size()));
            lamps.push_back({std::atoi(f[1].c_str()), std::atoi(f[2].c_str()), f[3]});
        } else if (f[0] == "flags" && f.size() >= 4) {
            for (int i = 0; i < 3; ++i) flags[i] = std::atoi(f[i + 1].c_str());
        }
    }
    if (lamps.empty() || !flags[0] || !flags[1] || !flags[2]) return 0;
    for (const Lamp& lamp : lamps) {
        menu_flags.push_back(lamp.flag);
        lit_flags.push_back(lamp.lit_flag);
    }
    *request = flags[0];
    *done = flags[1];
    *dream = flags[2];
    *lamp_flags = menu_flags.data();
    *lamp_lit_flags = lit_flags.data();
    return int(lamps.size());
}

extern "C" void bbgpu_lamp_menu_open(const unsigned char* lit_lamps, int here_lamp) {
    std::scoped_lock lock{mutex};
    for (size_t i = 0; i < lamps.size(); ++i) lit[i] = lit_lamps[i] != 0;
    here = here_lamp;
    tab = 0;
    for (int t = 0; t < int(tabs.size()); ++t) {
        if (here >= 0 && std::find(tabs[t].lamps.begin(), tabs[t].lamps.end(), here) != tabs[t].lamps.end()) tab = t;
    }
    if (!TabVisible(tab)) {
        for (int t = 0; t < int(tabs.size()); ++t) {
            if (TabVisible(t)) {
                tab = t;
                break;
            }
        }
    }
    FirstSelectable();
    result = BBGPU_LAMP_PENDING;
    detect_device = true;
    stick_x_held = stick_y_held = false;
    shown = true;
    std::printf("Lamp travel: menu opened (%d lamps lit)\n", int(std::count(lit, lit + lamps.size(), true)));
}

extern "C" void bbgpu_lamp_menu_close(void) {
    std::scoped_lock lock{mutex};
    shown = false;
    result = BBGPU_LAMP_PENDING;
}

extern "C" int bbgpu_lamp_menu_result(void) {
    std::scoped_lock lock{mutex};
    if (shown) return BBGPU_LAMP_PENDING;
    const int value = result;
    result = BBGPU_LAMP_PENDING;
    return value;
}

extern "C" void bbgpu_retry_prompt_open(const char* boss, const char* question, const char* retry,
                                        const char* decline) {
    std::scoped_lock lock{mutex};
    prompt_boss = boss ? boss : "";
    prompt_question = question ? question : "";
    prompt_retry = retry ? retry : "";
    prompt_decline = decline ? decline : "";
    prompt_result = BBGPU_RETRY_PENDING;
    detect_device = true;
    prompt_shown = true;
    std::printf("Boss retry: asking about %s\n", prompt_boss.c_str());
}

extern "C" void bbgpu_retry_prompt_close(void) {
    std::scoped_lock lock{mutex};
    prompt_shown = false;
    prompt_result = BBGPU_RETRY_PENDING;
}

extern "C" int bbgpu_retry_prompt_result(void) {
    std::scoped_lock lock{mutex};
    if (prompt_shown) return BBGPU_RETRY_PENDING;
    const int value = prompt_result;
    prompt_result = BBGPU_RETRY_PENDING;
    return value;
}
