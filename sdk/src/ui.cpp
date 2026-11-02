// The in-game overlay: Dear ImGui drawn on the game's D3D11 swap chain.
//
// Ported from MECMP's overlay (src/ui/overlay.cpp), trimmed to what an SDK needs:
//   - DXGI's Present and ResizeBuffers are found through a throwaway swap chain and swapped in
//     the swap chain vtable (dxgi.dll, not the game image; game code is never touched).
//   - ImGui starts on the first real Present. The game's window procedure is subclassed for
//     input; while the menu is open the mouse moves a virtual cursor (the game hides and
//     re-centres the real one and reads raw input), key presses go to ImGui only, and key
//     releases still reach the game so nothing stays held.
//   - Window messages are queued on the window thread and handed to ImGui on the render thread.
//   - The game's render targets are restored after GridNode draws.
// Mods draw in the same frame through on_ui (api.h, gridnode/ui.hpp).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/version.hpp"
#include "host.hpp"
#include "imgui.h"
#include "imgui_internal.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace gn::host::ui {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
PresentFn g_origPresent = nullptr;
ResizeFn g_origResize = nullptr;

Settings g_settings;
IDXGISwapChain* g_swapChain = nullptr; // the game's (the first one presented with a real window)
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_window = nullptr;
WNDPROC g_origWndProc = nullptr;
bool g_ready = false;
std::atomic<ImGuiContext*> g_imgui{nullptr};
std::atomic<bool> g_open{false};
std::atomic<bool> g_justOpened{false};
std::atomic<bool> g_typing{false}; // a text box has the keyboard (render thread writes, window thread reads)

// ---- mods' UI callbacks -----------------------------------------------------------------------
struct UiListener {
    std::string mod;
    gn_ui_fn fn;
    void* user;
};
std::mutex g_uiM;
std::vector<UiListener> g_ui; // guarded by g_uiM

// ---- keys ------------------------------------------------------------------------------------
struct KeyName { const char* name; int vk; };
const KeyName kKeyNames[] = {
    {"INSERT", VK_INSERT}, {"HOME", VK_HOME}, {"END", VK_END}, {"DELETE", VK_DELETE}, {"PAGEUP", VK_PRIOR},
    {"PAGEDOWN", VK_NEXT}, {"PAUSE", VK_PAUSE}, {"SCROLLLOCK", VK_SCROLL},
    {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4}, {"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7},
    {"F8", VK_F8}, {"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
    {"NUM0", VK_NUMPAD0}, {"NUM1", VK_NUMPAD1}, {"NUM2", VK_NUMPAD2}, {"NUM3", VK_NUMPAD3}, {"NUM4", VK_NUMPAD4},
    {"NUM5", VK_NUMPAD5}, {"NUM6", VK_NUMPAD6}, {"NUM7", VK_NUMPAD7}, {"NUM8", VK_NUMPAD8}, {"NUM9", VK_NUMPAD9},
    {"BACKTICK", VK_OEM_3}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"UP", VK_UP}, {"DOWN", VK_DOWN},
    {"BACKSPACE", VK_BACK}, {"TAB", VK_TAB}, {"ENTER", VK_RETURN}, {"SPACE", VK_SPACE},
    {"LBRACKET", VK_OEM_4}, {"RBRACKET", VK_OEM_6}, {"COMMA", VK_OEM_COMMA}, {"PERIOD", VK_OEM_PERIOD},
    {"SLASH", VK_OEM_2}, {"SEMICOLON", VK_OEM_1}, {"QUOTE", VK_OEM_7}, {"MINUS", VK_OEM_MINUS}, {"EQUALS", VK_OEM_PLUS},
    {"MOUSE3", VK_MBUTTON}, {"MOUSE4", VK_XBUTTON1}, {"MOUSE5", VK_XBUTTON2},
};

bool typesCharacter(int vk) { return MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) != 0; }

void createRtv() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

bool isInputMessage(UINT msg) {
    return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_INPUT;
}
bool isMouseMessage(UINT msg) { return msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST; }

// ---- virtual cursor (MECMP) ---------------------------------------------------------------------
// In gameplay the game hides the cursor, re-centres it and reads the mouse as raw input. While the
// menu is open GridNode moves its own cursor with the raw movement (WM_INPUT); the legacy mouse
// messages are only used when no raw input arrives (e.g. in the game's own menus).
std::atomic<float> g_cursorX{0}, g_cursorY{0};
std::atomic<bool> g_rawMouse{false};
std::atomic<float> g_mouseScale{1.f}; // Windows' pointer speed factor (raw input does not include it)
std::atomic<uint32_t> g_btnDown{0}, g_btnUp{0};
std::atomic<int> g_wheel{0};
// Free-camera look (layouts): while on, raw mouse movement turns the camera, not the cursor.
std::atomic<bool> g_lookCapture{false};
std::atomic<bool> g_pointer{false}; // the free camera's pointer mode: the cursor shows with the menu closed
std::atomic<float> g_lookX{0}, g_lookY{0};
// Free camera with the menu closed: the game gets no keyboard or mouse, GridNode takes them as if
// the menu were open (keys still reach ImGui), and the mouse always looks.
std::atomic<bool> g_capture{false};

void refreshMouseScale() {
    int speed = 10;
    if (!SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &speed, 0)) speed = 10;
    speed = std::clamp(speed, 1, 20);
    const float win = speed == 1 ? 1.f / 32.f : speed == 2 ? 1.f / 16.f : speed <= 10 ? (speed - 2) / 8.f : 1.f + (speed - 10) * 0.25f;
    g_mouseScale = win;
}

void clampCursor() {
    RECT rc{};
    GetClientRect(g_window, &rc);
    g_cursorX = std::clamp(g_cursorX.load(), 0.f, static_cast<float>(std::max<LONG>(rc.right - 1, 0)));
    g_cursorY = std::clamp(g_cursorY.load(), 0.f, static_cast<float>(std::max<LONG>(rc.bottom - 1, 0)));
}

// Window messages for ImGui are queued here (window thread) and delivered before NewFrame
// (render thread): ImGui is not thread safe.
struct ImGuiMsg { HWND hwnd; UINT msg; WPARAM w; LPARAM l; };
std::mutex g_msgM;
std::vector<ImGuiMsg> g_msgs; // guarded by g_msgM
constexpr size_t kMaxMsgs = 256;

void queueImGui(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    std::lock_guard<std::mutex> lock(g_msgM);
    if (g_msgs.size() < kMaxMsgs) g_msgs.push_back({hwnd, msg, w, l});
}

void deliverImGuiMsgs() {
    std::vector<ImGuiMsg> msgs;
    {
        std::lock_guard<std::mutex> lock(g_msgM);
        msgs.swap(g_msgs);
    }
    for (const ImGuiMsg& m : msgs) ImGui_ImplWin32_WndProcHandler(m.hwnd, m.msg, m.w, m.l);
}

void handleRawMouse(LPARAM l) {
    RAWINPUT ri{};
    UINT size = sizeof(ri);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) return;
    if (ri.header.dwType != RIM_TYPEMOUSE) return;
    const RAWMOUSE& m = ri.data.mouse;
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE) && (m.lLastX || m.lLastY) && g_lookCapture.load()) {
        g_lookX = g_lookX + static_cast<float>(m.lLastX);
        g_lookY = g_lookY + static_cast<float>(m.lLastY);
    } else if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE) && (m.lLastX || m.lLastY)) {
        g_cursorX = g_cursorX + static_cast<float>(m.lLastX) * g_mouseScale.load();
        g_cursorY = g_cursorY + static_cast<float>(m.lLastY) * g_mouseScale.load();
        clampCursor();
    }
    const USHORT b = m.usButtonFlags;
    uint32_t down = 0, up = 0;
    if (b & RI_MOUSE_LEFT_BUTTON_DOWN) down |= 1;
    if (b & RI_MOUSE_LEFT_BUTTON_UP) up |= 1;
    if (b & RI_MOUSE_RIGHT_BUTTON_DOWN) down |= 2;
    if (b & RI_MOUSE_RIGHT_BUTTON_UP) up |= 2;
    if (b & RI_MOUSE_MIDDLE_BUTTON_DOWN) down |= 4;
    if (b & RI_MOUSE_MIDDLE_BUTTON_UP) up |= 4;
    if (b & RI_MOUSE_BUTTON_4_DOWN) down |= 8;
    if (b & RI_MOUSE_BUTTON_4_UP) up |= 8;
    if (b & RI_MOUSE_BUTTON_5_DOWN) down |= 16;
    if (b & RI_MOUSE_BUTTON_5_UP) up |= 16;
    if (down) g_btnDown |= down;
    if (up) g_btnUp |= up;
    if (b & RI_MOUSE_WHEEL) g_wheel += static_cast<SHORT>(m.usButtonData);
    g_rawMouse = true;
}

// While the free camera flies, or the menu is open with raw mouse input (which moves GridNode's own
// cursor), the hidden system cursor stays in the middle of the game's window, so a click cannot
// land on another window.
void holdCursor(bool hold) {
    static bool held = false;
    if (hold && g_window && GetForegroundWindow() == g_window) {
        RECT rc{};
        GetClientRect(g_window, &rc);
        POINT tl{rc.left, rc.top}, br{rc.right, rc.bottom};
        ClientToScreen(g_window, &tl);
        ClientToScreen(g_window, &br);
        const RECT clip{tl.x, tl.y, br.x, br.y};
        ClipCursor(&clip);
        SetCursorPos((tl.x + br.x) / 2, (tl.y + br.y) / 2);
        held = true;
    } else if (held) {
        ClipCursor(nullptr);
        held = false;
    }
}

void applyRawMouse() {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(g_cursorX, g_cursorY);
    const uint32_t down = g_btnDown.exchange(0), up = g_btnUp.exchange(0);
    for (int i = 0; i < 5; ++i) {
        if (down & (1u << i)) io.AddMouseButtonEvent(i, true);
        if (up & (1u << i)) {
            if (down & (1u << i)) g_btnUp |= (1u << i); // down and up between two frames: release next frame
            else io.AddMouseButtonEvent(i, false);
        }
    }
    if (const int w = g_wheel.exchange(0)) io.AddMouseWheelEvent(0.f, static_cast<float>(w) / WHEEL_DELTA);
}

// 1 = key press, 2 = key release, 0 = not the keyboard.
int rawKeyEvent(LPARAM l) {
    RAWINPUT ri{};
    UINT size = sizeof(ri);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return 0;
    if (ri.header.dwType != RIM_TYPEKEYBOARD) return 0;
    return (ri.data.keyboard.Flags & RI_KEY_BREAK) ? 2 : 1;
}

std::atomic<int> g_releaseState{0}; // a full unload (release()): 0 running, 1 wanted, 2 under way, 3 done

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    // Left in place by a window procedure set over it: the overlay is gone, the game gets everything.
    if (g_releaseState.load() >= 2) return CallWindowProcW(g_origWndProc, hwnd, msg, w, l);
    // Alt+F4 always reaches the game.
    if (msg == WM_SYSKEYDOWN && static_cast<int>(w) == VK_F4) return CallWindowProcW(g_origWndProc, hwnd, msg, w, l);
    const bool keyUp = msg == WM_KEYUP || msg == WM_SYSKEYUP;
    if (keyUp && g_ready) queueImGui(hwnd, msg, w, l);
    const bool keyDown = msg == WM_KEYDOWN || (msg == WM_SYSKEYDOWN && w == VK_F10);
    // The menu key (not when it would type into a text box of the menu).
    if (keyDown && static_cast<int>(w) == g_settings.menuKey && !(l & (1 << 30)) && g_ready &&
        !(g_open.load() && g_typing.load() && typesCharacter(static_cast<int>(w)))) {
        toggleMenu();
        return 0;
    }
    if (msg == WM_DEVICECHANGE && g_ready) queueImGui(hwnd, msg, w, l);
    if ((g_open || g_capture) && g_ready) {
        // Key releases still reach the game, so a key held when the menu opened is released there.
        if (keyUp) return CallWindowProcW(g_origWndProc, hwnd, msg, w, l);
        if (msg == WM_INPUT) {
            const int key = rawKeyEvent(l);
            if (key == 2) return CallWindowProcW(g_origWndProc, hwnd, msg, w, l);
            if (key == 1) return DefWindowProcW(hwnd, msg, w, l); // a key press: swallowed
            handleRawMouse(l);
            return DefWindowProcW(hwnd, msg, w, l); // the game does not see the mouse
        }
        if (!isMouseMessage(msg) || !g_rawMouse) queueImGui(hwnd, msg, w, l);
        if (isInputMessage(msg)) return 0;
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, w, l);
}

// ---- fonts (MECMP): Segoe UI sized by the window height, rebuilt on big changes -----------------
float g_bakedPx = 0.f, g_wantPx = 0.f;
DWORD g_wantSince = 0;

float targetFontPx() {
    RECT rc{};
    float h = 0.f;
    if (g_window && GetClientRect(g_window, &rc)) h = static_cast<float>(rc.bottom - rc.top);
    if (h <= 0.f && g_wantPx > 0.f) return g_wantPx;
    float px = h > 0.f ? h / 60.f : 16.f;
    if (px < 15.f) px = 15.f;
    return std::clamp(px * std::clamp(g_settings.scale, 0.5f, 2.f), 8.f, 96.f);
}

std::string windowsFont(const wchar_t* file) {
    wchar_t dir[MAX_PATH];
    const UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    const std::wstring path = std::wstring(dir) + L"\\Fonts\\" + file;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &out[0], bytes, nullptr, nullptr);
    out.resize(static_cast<size_t>(bytes - 1));
    return out;
}

void buildFonts(float px) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFont* font = nullptr;
    const std::string segoe = windowsFont(L"segoeui.ttf");
    if (!segoe.empty()) font = io.Fonts->AddFontFromFileTTF(segoe.c_str(), px);
    if (!font) {
        ImFontConfig fc;
        fc.SizePixels = px;
        io.Fonts->AddFontDefault(&fc);
    }
    g_bakedPx = g_wantPx = px;
    g_wantSince = GetTickCount();
}

void updateFontScale() {
    const float want = targetFontPx();
    const DWORD now = GetTickCount();
    if (want != g_wantPx) {
        g_wantPx = want;
        g_wantSince = now;
    }
    const float ratio = want / g_bakedPx;
    if ((ratio < 0.8f || ratio > 1.25f) && now - g_wantSince > 400) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        buildFonts(want);
    }
    ImGui::GetIO().FontGlobalScale = want / g_bakedPx;
}

void applyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 6.f;
    s.FrameRounding = 4.f;
    s.GrabRounding = 4.f;
    s.WindowBorderSize = 1.f;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.10f, 0.94f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.75f, 0.20f, 0.12f, 1.f); // Catalyst red
    c[ImGuiCol_Header] = ImVec4(0.75f, 0.20f, 0.12f, 0.45f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.75f, 0.20f, 0.12f, 0.70f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.75f, 0.20f, 0.12f, 0.90f);
    c[ImGuiCol_Button] = ImVec4(0.75f, 0.20f, 0.12f, 0.55f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.85f, 0.28f, 0.18f, 0.85f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.95f, 0.32f, 0.20f, 1.f);
    c[ImGuiCol_CheckMark] = ImVec4(1.f, 0.45f, 0.30f, 1.f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.85f, 0.28f, 0.18f, 1.f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.75f, 0.20f, 0.12f, 0.50f);
    c[ImGuiCol_Tab] = ImVec4(0.45f, 0.14f, 0.10f, 0.80f);
    c[ImGuiCol_TabHovered] = ImVec4(0.85f, 0.28f, 0.18f, 0.85f);
    c[ImGuiCol_TabSelected] = ImVec4(0.75f, 0.20f, 0.12f, 1.f);
}

bool init(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow) return false;
    // Only the game's own window (a launcher overlay or another tool may present too).
    DWORD pid = 0;
    GetWindowThreadProcessId(desc.OutputWindow, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(desc.OutputWindow)) return false;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))) || !g_device) return false;
    g_device->GetImmediateContext(&g_context);
    g_swapChain = sc;
    g_window = desc.OutputWindow;
    createRtv();

    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // nothing written next to the game
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // A mod that leaves a window open (a missing End) must not take the overlay down.
    io.ConfigErrorRecovery = true;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    buildFonts(targetFontPx());
    applyStyle();
    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX11_Init(g_device, g_context);
    // The game's window procedure is stored before ours goes in: the window thread may call ours
    // at once, and it passes everything on to g_origWndProc.
    g_origWndProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_window, GWLP_WNDPROC));
    SetLastError(0);
    const LONG_PTR previous = SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wndProc));
    if (!previous && GetLastError() != 0)
        GN_LOG("ui: could not take the game window's input (error %lu); the menu key will not work", GetLastError());
    else if (previous && reinterpret_cast<WNDPROC>(previous) != g_origWndProc)
        g_origWndProc = reinterpret_cast<WNDPROC>(previous);
    g_imgui = ctx;
    RECT rc{};
    GetClientRect(g_window, &rc);
    GN_LOG("ui: overlay ready (%ldx%ld, Dear ImGui %s). Press %s in game for the GridNode menu.", rc.right - rc.left,
           rc.bottom - rc.top, IMGUI_VERSION, keyName(g_settings.menuKey).c_str());
    return true;
}

// The GridNode window (while the menu is open).
bool g_showInspector = false;
bool g_showConsole = false;
bool g_showMetrics = false;
bool g_showLayouts = false;
bool g_showRecorder = false;

void drawMain() {
    ImGui::SetNextWindowPos(ImVec2(30, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 22, 0), ImGuiCond_FirstUseEver);
    if (g_justOpened.exchange(false)) ImGui::SetNextWindowFocus();
    if (ImGui::Begin("GridNode " GRIDNODE_VERSION_STRING "###gridnode")) {
        ImGui::TextDisabled("%s closes this menu", keyName(g_settings.menuKey).c_str());
        const std::string level = levelName();
        ImGui::Text("Level: %s", level.empty() ? "(none)" : level.c_str());
        ImGui::Text("%.0f fps", ImGui::GetIO().Framerate);
        ImGui::Separator();
        if (g_settings.inspector) ImGui::Checkbox("Inspector", &g_showInspector);
        if (g_settings.console) ImGui::Checkbox("Console", &g_showConsole);
        if (g_settings.editor) ImGui::Checkbox("Layout editor", &g_showLayouts);
        if (g_settings.recorder) ImGui::Checkbox("Recorder", &g_showRecorder);
        if (g_settings.devInfo) ImGui::Checkbox("Dear ImGui metrics", &g_showMetrics);
        if (g_settings.inspector || g_settings.console || g_settings.editor || g_settings.recorder || g_settings.devInfo) ImGui::Separator();
        if (ImGui::CollapsingHeader("Mods", ImGuiTreeNodeFlags_DefaultOpen)) {
            const std::vector<ModInfo> mods = modList();
            if (mods.empty()) ImGui::TextDisabled("No mods loaded.");
            for (const ModInfo& m : mods) {
                ImGui::PushID(m.file.c_str());
                ImGui::BulletText("%s", m.file.c_str());
                ImGui::SameLine();
                if (m.loaded) ImGui::TextDisabled("loaded");
                else ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f), m.result ? "failed (%d)" : "unloaded", m.result);
                if (m.reloadable) {
                    // A mod that exports gn_mod_unload: rebuilt while the game runs, then reloaded.
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reload")) modReload(m.file);
                    ImGui::SameLine();
                    bool autoReload = m.autoReload;
                    if (ImGui::Checkbox("when changed", &autoReload)) modAutoReload(m.file, autoReload);
                    if (!m.status.empty()) ImGui::TextDisabled("    %s", m.status.c_str());
                }
                ImGui::PopID();
            }
        }
        if (ImGui::CollapsingHeader("Unload")) {
            static bool sure = false;
            if (unloadRequested()) {
                ImGui::TextDisabled("Unloading...");
            } else {
                ImGui::TextWrapped("Unloads every mod and GridNode itself and puts back what they changed (the free camera, "
                                   "god mode, hooks, this overlay). The game carries on as if GridNode had never loaded. "
                                   "Props moved by a saved layout stay until the level reloads.");
                ImGui::Checkbox("I am sure", &sure);
                ImGui::BeginDisabled(!sure);
                if (ImGui::Button("Unload GridNode and every mod")) unloadRequest();
                ImGui::EndDisabled();
                ImGui::TextDisabled("To load it again: gridnode_inject.exe, or restart the game.");
            }
        }
        if (g_settings.devInfo && ImGui::CollapsingHeader("World")) {
            void* cc = localClientCharacter();
            void* sc = localServerCharacter();
            gn_transform t{};
            ImGui::Text("Client character %p", cc);
            if (cc && entityTransform(cc, t)) ImGui::Text("  at %.2f, %.2f, %.2f", t.trans[0], t.trans[1], t.trans[2]);
            else if (cc) ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f), "  position unreadable");
            ImGui::Text("Server character %p", sc);
            if (sc && entityTransform(sc, t)) ImGui::Text("  at %.2f, %.2f, %.2f", t.trans[0], t.trans[1], t.trans[2]);
            else if (sc) ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f), "  position unreadable");
        }
        if (g_settings.devInfo && ImGui::CollapsingHeader("Game image")) {
            ImGui::TextWrapped("Saves the running game's code and data as a file (dumps\\*.exe next to gridnode.dll) for "
                               "gridnode.py fingerprint and verify-image. About 115 MB. Never share or commit it.");
            static std::atomic<int> state{0}; // 0 idle, 1 writing, 2 done, 3 failed
            static std::mutex textM;
            static std::string text;
            ImGui::BeginDisabled(state.load() == 1);
            if (ImGui::Button("Dump the game image")) {
                state = 1;
                std::thread([] {
                    BusyThread busy; // a full unload waits for the dump
                    wchar_t mod[MAX_PATH] = {};
                    HMODULE self = nullptr;
                    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(&mem::dumpImage), &self);
                    GetModuleFileNameW(self, mod, MAX_PATH);
                    std::wstring dir(mod);
                    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"dumps";
                    CreateDirectoryW(dir.c_str(), nullptr);
                    SYSTEMTIME t;
                    GetLocalTime(&t);
                    wchar_t name[64];
                    swprintf(name, 64, L"\\MirrorsEdgeCatalyst-%04u%02u%02u-%02u%02u%02u.exe", t.wYear, t.wMonth, t.wDay, t.wHour,
                             t.wMinute, t.wSecond);
                    const std::wstring path = dir + name;
                    std::string why;
                    // The game's own pointers in the slots GridNode hooked (the first hook's "previous"; a slot
                    // another mod had hooked first keeps that mod's pointer, and the check reports it as such).
                    std::vector<std::pair<uintptr_t, uint64_t>> restore;
                    for (const auto& [slot, chain] : hooks().snapshot())
                        if (!chain.empty()) restore.push_back({static_cast<uintptr_t>(slot), chain.front().previous});
                    const bool ok = mem::dumpImage(path, &why, restore);
                    std::lock_guard<std::mutex> lock(textM);
                    char buf[600];
                    if (ok) snprintf(buf, sizeof(buf), "Written: %ls", path.c_str());
                    else snprintf(buf, sizeof(buf), "Failed: %s", why.c_str());
                    text = buf;
                    state = ok ? 2 : 3;
                }).detach();
            }
            ImGui::EndDisabled();
            if (state.load() == 1) ImGui::TextDisabled("Writing...");
            else if (state.load() >= 2) {
                std::lock_guard<std::mutex> lock(textM);
                ImGui::TextWrapped("%s", text.c_str());
            }
        }
        if (g_settings.devInfo && ImGui::CollapsingHeader("Hooks")) {
            ImGui::Text("%llu pointer hooks installed", static_cast<unsigned long long>(hooks().count()));
            ImGui::Text("Server ticks: %llu", static_cast<unsigned long long>(server().ticks()));
            ImGui::Text("Client ticks: %llu", static_cast<unsigned long long>(client().ticks()));
        }
    }
    ImGui::End();
    if (g_showInspector && g_settings.inspector) inspector::draw(&g_showInspector);
    if (g_showConsole && g_settings.console) inspector::drawConsole(&g_showConsole);
    if (g_showMetrics && g_settings.devInfo) ImGui::ShowMetricsWindow(&g_showMetrics);
    if (g_showLayouts && g_settings.editor) layouts::draw(&g_showLayouts);
    if (g_showRecorder && g_settings.recorder) recorder::draw(&g_showRecorder);
}

void callMods(bool open) {
    std::vector<UiListener> ls;
    {
        std::lock_guard<std::mutex> lock(g_uiM);
        ls = g_ui;
    }
    for (const UiListener& l : ls) {
        ImGuiErrorRecoveryState state;
        ImGui::ErrorRecoveryStoreState(&state);
        try {
            l.fn(open ? 1 : 0, l.user);
        } catch (...) {
            static int said = 0;
            if (said++ < 10) GN_LOG("ui: a mod's UI callback threw; ignored");
        }
        ImGui::ErrorRecoveryTryToRecoverState(&state); // closes what the mod left open
        ImGui::SetCurrentContext(g_imgui.load());     // in case the mod switched it
    }
}

std::atomic<uint64_t> g_frames{0}; // frames drawn (a mod being unloaded waits for a few)

// A full unload (release()): the render thread closes the overlay and puts the slots back at its
// next Present; with no frames coming (minimised), GridNode's thread puts them back without closing
// Dear ImGui (its device objects belong to the render thread).
void** g_presentSlot = nullptr;
void** g_resizeSlot = nullptr;
std::atomic<bool> g_releaseOk{true};
HRESULT STDMETHODCALLTYPE presentHook(IDXGISwapChain* sc, UINT sync, UINT flags);
HRESULT STDMETHODCALLTYPE resizeHook(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags);

void shutdownOverlay() { // render thread
    if (!g_ready) return;
    ImGui::SetCurrentContext(g_imgui.load());
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_imgui = nullptr;
    if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
    if (g_context) g_context->Release(), g_context = nullptr;
    if (g_device) g_device->Release(), g_device = nullptr;
    g_ready = false;
}

void putBack() {
    if (g_window && g_origWndProc) {
        if (GetWindowLongPtrW(g_window, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&wndProc)) {
            SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        } else {
            GN_LOG("ui: the game window's procedure was replaced after GridNode's; it stays (GridNode stays loaded)");
            g_releaseOk = false;
        }
    }
    g_open = false, g_capture = false, g_lookCapture = false, g_pointer = false;
    ClipCursor(nullptr);
    auto slot = [](void** at, void* mine, void* original, const char* what) {
        if (!at) return;
        if (*at == mine) {
            uintptr_t old = 0;
            if (mem::patchModulePointer(reinterpret_cast<uintptr_t>(at), reinterpret_cast<uintptr_t>(original), &old)) return;
            GN_LOG("ui: %s could not be put back", what);
        } else {
            GN_LOG("ui: %s was hooked again after GridNode (another overlay); it stays (GridNode stays loaded)", what);
        }
        g_releaseOk = false;
    };
    slot(g_presentSlot, reinterpret_cast<void*>(&presentHook), reinterpret_cast<void*>(g_origPresent), "Present");
    slot(g_resizeSlot, reinterpret_cast<void*>(&resizeHook), reinterpret_cast<void*>(g_origResize), "ResizeBuffers");
}

HRESULT STDMETHODCALLTYPE presentHook(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (g_releaseState.load() != 0) {
        if (int wanted = 1; g_releaseState.compare_exchange_strong(wanted, 2)) {
            shutdownOverlay();
            putBack();
            g_releaseState = 3;
        }
        return g_origPresent(sc, sync, flags);
    }
    if (flags & DXGI_PRESENT_TEST) return g_origPresent(sc, sync, flags);
    if (!g_ready && !g_swapChain) g_ready = init(sc);
    if (g_ready && sc == g_swapChain && g_rtv) {
        ImGui::SetCurrentContext(g_imgui.load());
        updateFontScale();
        ImGui_ImplDX11_NewFrame();
        deliverImGuiMsgs();
        ImGui_ImplWin32_NewFrame();
        if ((g_open || g_capture) && g_rawMouse) applyRawMouse(); // after the backend's own cursor read, so ours wins
        ImGui::NewFrame();
        const bool open = g_open.load();
        {
            ImGuiErrorRecoveryState state;
            ImGui::ErrorRecoveryStoreState(&state);
            try {
                layouts::frame(open, g_settings.editor); // the free camera (mods use it too); with the editor, its keys
                if (g_settings.recorder) recorder::frame();
                hotkeys::frame(open);
            } catch (...) { // never into the game's Present
                ImGui::ErrorRecoveryTryToRecoverState(&state);
                static int said = 0;
                if (said++ < 10) GN_LOG("ui: an editor or recorder frame threw; ignored");
            }
        }
        holdCursor((g_capture.load() && !open) || (open && g_rawMouse.load())); // raw input moves our cursor, so the system one can stay put
        ImGui::GetIO().MouseDrawCursor = open || (g_capture.load() && g_pointer.load());
        {
            static DWORD scaleAt = 0;
            if (GetTickCount() - scaleAt > 500) {
                scaleAt = GetTickCount();
                refreshMouseScale();
            }
        }
        if (open) {
            ImGuiErrorRecoveryState state;
            ImGui::ErrorRecoveryStoreState(&state);
            try {
                drawMain();
            } catch (...) {
                ImGui::ErrorRecoveryTryToRecoverState(&state);
                static int said = 0;
                if (said++ < 10) GN_LOG("ui: the menu threw while drawing; ignored");
            }
        }
        callMods(open);
        g_frames.fetch_add(1, std::memory_order_relaxed);
        g_typing = ImGui::GetIO().WantTextInput;
        ImGui::Render();
        // The backend restores most pipeline state, but not the render targets: put the game's back.
        ID3D11RenderTargetView* gameRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* gameDsv = nullptr;
        g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, gameRtvs, &gameDsv);
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, gameRtvs, gameDsv);
        for (ID3D11RenderTargetView* v : gameRtvs)
            if (v) v->Release();
        if (gameDsv) gameDsv->Release();
    }
    return g_origPresent(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE resizeHook(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    if (g_releaseState.load() != 0) return g_origResize(sc, count, w, h, fmt, flags);
    if (sc == g_swapChain && g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
    const HRESULT hr = g_origResize(sc, count, w, h, fmt, flags);
    if (sc == g_swapChain && g_ready) createRtv();
    return hr;
}

// Swaps one slot of DXGI's swap chain vtable, keeping what it held (another overlay's hook, if one
// was installed first, is chained).
template <class Fn> bool swapSlot(void** slot, Fn detour, Fn* original) {
    *original = reinterpret_cast<Fn>(*slot); // published before the swap
    uintptr_t old = 0;
    if (!mem::patchModulePointer(reinterpret_cast<uintptr_t>(slot), reinterpret_cast<uintptr_t>(detour), &old)) {
        GN_LOG("ui: swap chain slot %p is not in a loaded module's data; not hooking it", static_cast<void*>(slot));
        return false;
    }
    *original = reinterpret_cast<Fn>(old);
    return true;
}

} // namespace

std::string keyName(int vk) {
    for (const KeyName& k : kKeyNames)
        if (k.vk == vk) return k.name;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    char buf[8];
    snprintf(buf, sizeof(buf), "VK_%02X", vk & 0xFF);
    return buf;
}

std::vector<std::pair<int, std::string>> reservedKeys() {
    std::vector<std::pair<int, std::string>> r{{g_settings.menuKey, "the menu"}};
    if (g_settings.recorder)
        r.push_back({VK_F8, "Recorder: record / pause"}), r.push_back({VK_HOME, "Recorder: marker"}),
            r.push_back({VK_END, "Recorder: save the last seconds"});
    if (g_settings.editor) r.push_back({VK_F7, "free camera (default)"});
    return r;
}

int keyFromName(const std::string& n) {
    if (n.size() == 1 && isalnum(static_cast<unsigned char>(n[0]))) return toupper(static_cast<unsigned char>(n[0]));
    for (const KeyName& k : kKeyNames)
        if (_stricmp(n.c_str(), k.name) == 0) return k.vk;
    if (n.size() > 3 && _strnicmp(n.c_str(), "VK_", 3) == 0) {
        char* end = nullptr;
        const long vk = strtol(n.c_str() + 3, &end, 16);
        if (end && *end == 0 && vk > 6 && vk < 0xFF && vk != VK_ESCAPE) return static_cast<int>(vk);
    }
    return 0;
}

void toggleMenu() {
    const bool open = !g_open.load();
    if (open && g_window) {
        ClipCursor(nullptr);
        RECT rc{};
        GetClientRect(g_window, &rc);
        g_cursorX = (rc.right - rc.left) * 0.5f;
        g_cursorY = (rc.bottom - rc.top) * 0.5f;
        g_rawMouse = false;
        g_justOpened = true;
    }
    g_open = open;
}

bool menuOpen() { return g_open.load(); }
bool gameFocused() { return g_window && GetForegroundWindow() == g_window; }
void setLookCapture(bool on) {
    if (on != g_lookCapture.exchange(on)) g_lookX = 0.f, g_lookY = 0.f;
}
void setInputCapture(bool on) { g_capture = on; }
void setPointer(bool on) {
    if (on && !g_pointer.load() && g_window) { // from the middle of the window, where the crosshair was
        RECT rc{};
        GetClientRect(g_window, &rc);
        g_cursorX = (rc.right - rc.left) * 0.5f;
        g_cursorY = (rc.bottom - rc.top) * 0.5f;
    }
    g_pointer = on;
}
void takeLook(float& dx, float& dy) {
    dx = g_lookX.exchange(0.f);
    dy = g_lookY.exchange(0.f);
}
void* context() { return g_imgui.load(); }

void allocators(void** alloc, void** free, void** user) {
    ImGuiMemAllocFunc a = nullptr;
    ImGuiMemFreeFunc f = nullptr;
    void* u = nullptr;
    ImGui::GetAllocatorFunctions(&a, &f, &u);
    if (alloc) *alloc = reinterpret_cast<void*>(a);
    if (free) *free = reinterpret_cast<void*>(f);
    if (user) *user = u;
}

int check(const char* mod, const char* version, size_t io, size_t style, size_t vec2, size_t vec4, size_t vert, size_t idx) {
    const bool same = version && strcmp(version, IMGUI_VERSION) == 0 && io == sizeof(ImGuiIO) && style == sizeof(ImGuiStyle) &&
                      vec2 == sizeof(ImVec2) && vec4 == sizeof(ImVec4) && vert == sizeof(ImDrawVert) && idx == sizeof(ImDrawIdx);
    if (!same)
        GN_LOG("[%s] ui: built with Dear ImGui %s (ImGuiIO %llu bytes), GridNode has %s (%llu): rebuild the mod with "
               "GridNode's third_party/imgui; its UI is off",
               mod ? mod : "?", version ? version : "?", static_cast<unsigned long long>(io), IMGUI_VERSION,
               static_cast<unsigned long long>(sizeof(ImGuiIO)));
    return same ? GN_OK : GN_ERR_MISMATCH;
}

int onUi(const std::string& mod, gn_ui_fn fn, void* user) {
    if (!fn) return GN_ERR_ARGUMENT;
    if (!g_settings.enabled) return GN_ERR_UNAVAILABLE;
    std::lock_guard<std::mutex> lock(g_uiM);
    g_ui.push_back({mod, fn, user});
    return GN_OK;
}

bool install(const Settings& s) {
    g_settings = s;
    if (!s.enabled) {
        GN_LOG("ui: disabled in gridnode.ini");
        return false;
    }
    // A throwaway device and swap chain on a hidden window, only to find DXGI's swap chain vtable
    // (shared by every swap chain of the process, the game's included).
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GridNode_probe";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr) || !sc) {
        GN_LOG("ui: could not create a probe swap chain (0x%08lx); no overlay", static_cast<unsigned long>(hr));
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }
    void** vtbl = *reinterpret_cast<void***>(sc);
    const bool ok = swapSlot(&vtbl[8], &presentHook, &g_origPresent) && swapSlot(&vtbl[13], &resizeHook, &g_origResize);
    if (ok) g_presentSlot = &vtbl[8], g_resizeSlot = &vtbl[13];
    sc->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (!ok) {
        GN_LOG("ui: could not hook Present; no overlay");
        return false;
    }
    GN_LOG("ui: Present hooked; the overlay starts with the next frame");
    return true;
}

void forgetUi(const CodeRange& r) {
    std::lock_guard<std::mutex> lock(g_uiM);
    g_ui.erase(std::remove_if(g_ui.begin(), g_ui.end(), [&](const UiListener& l) { return r.has(reinterpret_cast<void*>(l.fn)); }), g_ui.end());
}

uint64_t frameCount() { return g_frames.load(std::memory_order_relaxed); }

bool release() {
    if (!g_presentSlot) return true; // never hooked
    if (int running = 0; g_releaseState.compare_exchange_strong(running, 1)) {
        const ULONGLONG start = GetTickCount64();
        while (g_releaseState.load() != 3 && GetTickCount64() - start < 2000) Sleep(20);
        if (int wanted = 1; g_releaseState.compare_exchange_strong(wanted, 2)) {
            GN_LOG("ui: no frame came (the game minimised?): the slots go back without closing the overlay");
            putBack();
            g_releaseState = 3;
        }
    }
    while (g_releaseState.load() != 3) Sleep(10); // the render thread is at it
    return g_releaseOk.load();
}

} // namespace gn::host::ui
