// world-draw: the example for drawing in the world (API 10). Draws a box around Faith and labels the
// entities near her with their type, which also makes it a research view: what is there, and where.
//
//   - gn::ui::view: the camera as drawn this frame
//   - gn::ui::box3d / line3d / marker / text3d: shapes at world positions (cut behind the camera)
//   - gn::entitiesNear: the entities around her (read on the client tick, twice a second)
//   - gn::config: its options are kept in mods/world-draw.ini
//
// Options are in its window (INSERT). Threads: the client tick reads the game, the draw callback only
// draws what it last read.
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/ui.hpp"

namespace {

std::atomic<bool> g_faithBox{true}, g_near{true}, g_names{true};
std::atomic<float> g_radius{10.f};

struct Seen {
    gn::Vec3 at;
    std::string type;
    float distance;
};
std::mutex g_m;
std::vector<Seen> g_seen;      // (g_m)
gn::Vec3 g_faith{};            // (g_m)
bool g_haveFaith = false;      // (g_m)

void clientTick(float dt) {
    static float t = 0.f;
    if ((t += dt) < 0.5f) return;
    t = 0.f;
    void* faith = gn::localClientCharacter();
    gn::Vec3 p{};
    const bool have = faith && gn::positionOf(faith, p);
    std::vector<Seen> seen;
    if (have && g_near) {
        gn::entitiesNear(p, g_radius, [&](void* e, gn::Type type, const gn::Transform& at, float d) {
            if (e == faith || seen.size() >= 200) return; // a busy place stays readable
            seen.push_back({{at.trans[0], at.trans[1], at.trans[2]}, type ? type.name() : "?", d});
        });
    }
    std::lock_guard<std::mutex> lock(g_m);
    g_faith = p, g_haveFaith = have;
    g_seen = std::move(seen);
}

void draw(bool menuOpen) {
    gn::ui::View v;
    if (gn::ui::view(v)) {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_haveFaith && g_faithBox) {
            const gn::Vec3 f = g_faith; // her position is at her feet
            gn::ui::box3d(v, {f.x - 0.3f, f.y, f.z - 0.3f}, {f.x + 0.3f, f.y + 1.8f, f.z + 0.3f}, IM_COL32(60, 220, 255, 220));
            gn::ui::line3d(v, f, {f.x, f.y - 0.5f, f.z}, IM_COL32(60, 220, 255, 220)); // a tick under her feet
        }
        for (const Seen& s : g_seen) {
            gn::ui::marker(v, s.at, IM_COL32(255, 200, 60, 230), 3.f);
            if (!g_names) continue;
            char label[160];
            snprintf(label, sizeof(label), "%s %.1f m", s.type.c_str(), s.distance);
            gn::ui::text3d(v, {s.at.x, s.at.y + 0.25f, s.at.z}, label, IM_COL32(255, 230, 150, 255));
        }
    }
    if (!menuOpen) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 20, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("World draw")) {
        bool b = g_faithBox;
        if (ImGui::Checkbox("Box around Faith", &b)) g_faithBox = b, gn::config::setBool("faith_box", b);
        b = g_near;
        if (ImGui::Checkbox("Entities near her", &b)) g_near = b, gn::config::setBool("near", b);
        b = g_names;
        if (ImGui::Checkbox("Their types", &b)) g_names = b, gn::config::setBool("names", b);
        float r = g_radius;
        if (ImGui::SliderFloat("Radius (m)", &r, 1.f, 50.f, "%.0f")) g_radius = r, gn::config::setFloat("radius", r);
        size_t n;
        {
            std::lock_guard<std::mutex> lock(g_m);
            n = g_seen.size();
        }
        ImGui::TextDisabled("%zu entities shown (at most 200)", n);
        gn::ui::View now;
        if (gn::ui::view(now))
            ImGui::TextDisabled("camera (%.1f, %.1f, %.1f), fov %.0f, %.0f x %.0f", now.position[0], now.position[1], now.position[2],
                                now.fov_y, now.width, now.height);
        else
            ImGui::TextDisabled("no camera");
    }
    ImGui::End();
}

} // namespace

GN_MOD("world-draw") {
    g_faithBox = gn::config::getBool("faith_box", true);
    g_near = gn::config::getBool("near", true);
    g_names = gn::config::getBool("names", true);
    g_radius = gn::config::getFloat("radius", 10.f);
    if (!gn::ui::onDraw(draw)) {
        gn::log("no overlay: nothing to draw with");
        return false;
    }
    gn::onClientTick(clientTick);
    return true;
}

// Reloadable (GridNode menu, Mods: Reload, or when the DLL changes): nothing of its own to stop;
// GridNode takes out its callbacks.
GN_MOD_UNLOAD() { return true; }
