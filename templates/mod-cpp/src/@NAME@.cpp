// @NAME@: @DESCRIPTION@
//
// A GridNode mod. gridnode/mod.hpp is the API (docs/modding.md); gridnode/ui.hpp draws with Dear ImGui
// in GridNode's overlay; gridnode/engine.hpp has the game's addresses GridNode knows.
#include <atomic>
#include <cstdio>

#include "gridnode/ui.hpp"

namespace {

std::atomic<bool> g_on{true};
std::atomic<float> g_y{0.f}; // Faith's height, read on the client tick

void clientTick(float) {
    gn::Vec3 p{};
    if (void* faith = gn::localClientCharacter(); faith && gn::positionOf(faith, p)) g_y = p.y;
}

void draw(bool menuOpen) {
    if (!menuOpen) return; // its window is shown with GridNode's menu (INSERT)
    if (ImGui::Begin("@TITLE@")) {
        bool on = g_on;
        if (ImGui::Checkbox("On", &on)) g_on = on, gn::config::setBool("on", on);
        ImGui::Text("Faith is at height %.1f m", g_y.load());
    }
    ImGui::End();
}

} // namespace

GN_MOD_INFO("@ID@", "@TITLE@", "0.1.0", "@AUTHOR@", "@DESCRIPTION@");

GN_MOD("@ID@") {
    g_on = gn::config::getBool("on", true); // kept in mods\@ID@.ini
    gn::ui::onDraw(draw);
    gn::onClientTick(clientTick);
    gn::log("loaded from %s", gn::modDir().c_str());
    return true;
}

// Reloadable while the game runs (its DLL replaced, or GridNode's menu: Mods, Reload): GridNode takes
// out its callbacks, hooks, boxes and sounds; put back anything else it changed here.
GN_MOD_UNLOAD() { return true; }
