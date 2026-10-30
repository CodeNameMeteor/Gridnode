#pragma once
// Dear ImGui windows for GridNode mods, header-only.
//
//   #include <gridnode/ui.hpp>   // includes imgui.h: link the gn_imgui library (CMake)
//
//   GN_MOD("my-mod") {
//       gn::ui::onDraw([](bool menuOpen) {
//           if (!menuOpen) return;            // the mouse belongs to the game
//           ImGui::Begin("My mod");
//           ImGui::Text("Hello");
//           ImGui::End();
//       });
//       return true;
//   }
//
// The mod builds GridNode's copy of Dear ImGui (third_party/imgui, the gn_imgui CMake target), so
// its ImGui functions work on GridNode's context: before each callback this header points the
// mod's ImGui at GridNode's context and allocator. The version and data layout are checked once;
// a mod built against another ImGui gets no UI (logged) instead of corrupting the frame.
//
// Callbacks run on the render thread every frame. Do not call engine functions from them: hand
// work to the game with gn::runOnServer / gn::runOnClient, and share state through atomics or a
// mutex.

#include <functional>
#include <string>
#include <utility>

#include "gridnode/mod.hpp"
#include "imgui.h"

namespace gn::ui {

// GN_OK if this mod's Dear ImGui matches GridNode's (checked once).
inline bool compatible() {
    static const bool ok = api().ui_check(modName(), IMGUI_VERSION, sizeof(ImGuiIO), sizeof(ImGuiStyle), sizeof(ImVec2),
                                          sizeof(ImVec4), sizeof(ImDrawVert), sizeof(ImDrawIdx)) == GN_OK;
    return ok;
}

namespace detail {
inline bool bind() {
    auto* ctx = static_cast<ImGuiContext*>(api().ui_context());
    if (!ctx) return false;
    void *a = nullptr, *f = nullptr, *u = nullptr;
    api().ui_allocators(&a, &f, &u);
    ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(a), reinterpret_cast<ImGuiMemFreeFunc>(f), u);
    ImGui::SetCurrentContext(ctx);
    return true;
}
} // namespace detail

// fn(bool menuOpen) every frame, between NewFrame and Render. menuOpen: the GridNode menu is open
// (the overlay has the mouse and keyboard); draw interactive windows only then, HUD text any time.
template <class F> bool onDraw(F f) {
    if (!compatible()) return false;
    auto* p = ::gn::detail::keep(std::function<void(bool)>(std::move(f)));
    return api().on_ui(modName(),
                       [](int open, void* u) {
                           if (detail::bind()) (*static_cast<std::function<void(bool)>*>(u))(open != 0);
                       },
                       p) == GN_OK;
}

inline bool menuOpen() { return api().ui_menu_open() != 0; }

namespace detail {
// The hotkey's button (and "default" when changed); the label, if any, between them.
inline bool hotkeyParts(const char* id, const char* text, float width) {
    HotkeyInfo h;
    if (!hotkeyInfo(id, h)) return false;
    ImGui::PushID(id);
    const char* key = h.waiting ? "press a key..." : h.name;
    const bool clicked = ImGui::Button((std::string(key) + "###key").c_str(), ImVec2(width > 0.f ? width : ImGui::GetFontSize() * 6.f, 0.f));
    if (clicked) hotkeyRebind(id);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click, then press a key. Escape: no key. Backspace: the default.");
    if (text) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
    }
    if (h.vk != h.default_vk) {
        ImGui::SameLine();
        if (ImGui::SmallButton("default")) hotkeySet(id, -1);
    }
    if (h.clash[0]) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "  also on %s: %s", h.name, h.clash);
    ImGui::PopID();
    return clicked;
}
} // namespace detail

// A button showing the hotkey's key (gn::hotkey): click it, then press a key (Escape: none,
// Backspace: the default). "default" beside it when changed, and a warning under it when something
// else uses the key. Returns true when clicked.
inline bool hotkeyButton(const char* id, float width = 0.f) { return detail::hotkeyParts(id, nullptr, width); }

// A row for a list of keys: the hotkey's button, then its label.
inline bool hotkeyRow(const char* id, const char* label, float width = 0.f) { return detail::hotkeyParts(id, label, width); }

} // namespace gn::ui
