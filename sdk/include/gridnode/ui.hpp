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
#include "gridnode/projection.hpp"
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
                           if (detail::bind())
                               ::gn::detail::guarded("an onDraw callback", [&] { (*static_cast<std::function<void(bool)>*>(u))(open != 0); });
                       },
                       p) == GN_OK;
}

inline bool menuOpen() { return api().ui_menu_open() != 0; }
// Opens or closes the menu (API 27): give your mod its own key with gn::hotkey and open it from there.
inline bool setMenuOpen(bool open) { return api().ui_menu_set(open ? 1 : 0) == GN_OK; }

namespace detail {
// The free camera's key list window, to add to (false: it is not shown now).
inline bool freecamBegin() {
    if (menuOpen() || !freecamOn()) return false;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - pad, vp->Pos.y + pad), ImGuiCond_Always, ImVec2(1.f, 0.f));
    ImGui::Begin("##freecam", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    return true;
}
} // namespace detail

// A row in the free camera's key list (top right while it flies with the menu closed): a mod's own
// keys next to GridNode's. Call it from onDraw; GridNode draws the list first in the frame, so the
// rows go at its end. Does nothing while the list is not shown.
inline void freecamRow(const std::string& key, const char* what) {
    if (!detail::freecamBegin()) return;
    ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "%s", key.c_str());
    ImGui::SameLine(ImGui::GetFontSize() * 7.5f);
    ImGui::TextUnformatted(what);
    ImGui::End();
}
// A line between groups of rows, with a heading when text is given.
inline void freecamSeparator(const char* text = nullptr) {
    if (!detail::freecamBegin()) return;
    ImGui::Separator();
    if (text) ImGui::TextUnformatted(text);
    ImGui::End();
}

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

// ---- drawing in the world (API 10) -------------------------------------------------------------
// In a draw callback: shapes at world positions, drawn behind every window (the background draw
// list), with the camera as the game draws this frame. Lines are cut where they pass behind the
// camera; points behind it are not drawn. Colours are IM_COL32(r, g, b, a).
using View = gn_view;
// The camera this frame; false while there is none (loading, menus).
inline bool view(View& out) { return api().view_get_ex(&out, sizeof(out)) == GN_OK; }
// A world point on the screen (pixels); false when it is behind the camera.
inline bool toScreen(const View& v, const Vec3& p, ImVec2& out) {
    const float w[3] = {p.x, p.y, p.z};
    float s[2];
    if (!projection::toScreen(v, w, s)) return false;
    out = ImVec2(s[0], s[1]);
    return true;
}
inline void line3d(const View& v, const Vec3& a, const Vec3& b, ImU32 col, float thickness = 1.5f) {
    const float p[3] = {a.x, a.y, a.z}, q[3] = {b.x, b.y, b.z};
    float sa[2], sb[2];
    if (projection::segmentToScreen(v, p, q, sa, sb)) ImGui::GetBackgroundDrawList()->AddLine(ImVec2(sa[0], sa[1]), ImVec2(sb[0], sb[1]), col, thickness);
}
// A box from `mn` to `mx` in the frame of `t` (its local corners through the transform), or in world
// space when t is null.
inline void box3d(const View& v, const Vec3& mn, const Vec3& mx, ImU32 col, const Transform* t = nullptr, float thickness = 1.5f) {
    Vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        const float l[3] = {i & 1 ? mx.x : mn.x, i & 2 ? mx.y : mn.y, i & 4 ? mx.z : mn.z};
        c[i] = t ? Vec3{t->right[0] * l[0] + t->up[0] * l[1] + t->forward[0] * l[2] + t->trans[0],
                        t->right[1] * l[0] + t->up[1] * l[1] + t->forward[1] * l[2] + t->trans[1],
                        t->right[2] * l[0] + t->up[2] * l[1] + t->forward[2] * l[2] + t->trans[2]}
                 : Vec3{l[0], l[1], l[2]};
    }
    static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : kEdges) line3d(v, c[e[0]], c[e[1]], col, thickness);
}
// A dot of `radius` pixels at a world point.
inline void marker(const View& v, const Vec3& p, ImU32 col, float radius = 4.f) {
    ImVec2 s;
    if (toScreen(v, p, s)) ImGui::GetBackgroundDrawList()->AddCircleFilled(s, radius, col);
}
// Text centred on a world point.
inline void text3d(const View& v, const Vec3& p, const char* text, ImU32 col) {
    ImVec2 s;
    if (!toScreen(v, p, s)) return;
    const ImVec2 size = ImGui::CalcTextSize(text);
    ImGui::GetBackgroundDrawList()->AddText(ImVec2(s.x - size.x * 0.5f, s.y - size.y * 0.5f), col, text);
}

} // namespace gn::ui
