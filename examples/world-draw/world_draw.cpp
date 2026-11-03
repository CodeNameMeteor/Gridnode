// world-draw: the example for drawing in the world (API 10). Draws a box around Faith and labels the
// entities near her with their type, which also makes it a research view: what is there, and where.
//
//   - gn::ui::view: the camera as drawn this frame
//   - gn::ui::box3d / line3d / marker / text3d: shapes at world positions (cut behind the camera)
//   - gn::entitiesNear: the entities around her (read on the client tick, twice a second)
//   - gn::config: its options are kept in mods/world-draw.ini
//   - gn::raycastObject, entityBodies, bodyTransform / bodySetTransform (API 18): what the camera
//     looks at, its collision bodies, and lifting them (read on the server tick, four times a second)
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

// What the camera looks at (server tick).
std::atomic<bool> g_look{true};
std::atomic<int> g_liftWanted{0}; // 1: lift its bodies 1 m, -1: put them back
struct Looked {
    bool hit = false;
    gn::Vec3 at;
    std::string what;               // its type, or "level geometry"
    uint32_t body = 0, part = 0;
    std::vector<gn::Vec3> bodies;   // where its collision bodies are
};
Looked g_looked;                    // (g_m)
struct Lifted {
    void* entity;
    uint32_t body;
    gn_transform was;
};
std::vector<Lifted> g_lifted;       // (server thread) bodies moved, to put back
std::vector<Lifted> g_liftedClient; // (client thread) the same for the client half
std::atomic<void*> g_clientLift{nullptr};   // the client half to lift next client tick
std::atomic<bool> g_clientPutBack{false};
std::string g_liftNote, g_clientNote;       // (g_m)

// Each realm has its own physics (Faith collides with the client half): lift a half's bodies 1 m.
std::string lift(void* e, std::vector<Lifted>& lifted) {
    int ok = 0, n = 0;
    for (uint32_t b : gn::entityBodies(e)) {
        ++n;
        gn_transform was{};
        if (!gn::bodyTransform(e, b, was)) continue;
        gn_transform up = was;
        up.trans[1] += 1.f;
        const int r = gn::bodySetTransform(e, b, up);
        if (r == GN_OK) lifted.push_back({e, b, was}), ++ok;
        else gn::log("lift: body %08x: error %d", b, r);
    }
    return "lifted " + std::to_string(ok) + " of " + std::to_string(n) + " bodies 1 m";
}

std::string putBack(std::vector<Lifted>& lifted) {
    int n = 0;
    // Newest first: lifted twice, the first place is the one kept last.
    for (auto it = lifted.rbegin(); it != lifted.rend(); ++it) n += gn::bodySetTransform(it->entity, it->body, it->was) == GN_OK;
    const std::string note = "put back " + std::to_string(n) + " of " + std::to_string(lifted.size()) + " bodies";
    lifted.clear();
    return note;
}

void serverTick(float dt) {
    static float t = 0.f;
    if ((t += dt) < 0.25f) return;
    t = 0.f;
    if (const int want = g_liftWanted.exchange(0); want < 0) { // put back what was lifted
        const std::string note = "server: " + putBack(g_lifted);
        g_clientPutBack = true;
        gn::log("%s", note.c_str());
        std::lock_guard<std::mutex> lock(g_m);
        g_liftNote = note;
    } else if (want > 0) {
        void* e = nullptr;
        gn::ui::View v;
        gn::ObjectHit h{};
        if (gn::ui::view(v)) {
            const gn::Vec3 from{v.position[0], v.position[1], v.position[2]};
            const gn::Vec3 to{from.x + v.forward[0] * 200.f, from.y + v.forward[1] * 200.f, from.z + v.forward[2] * 200.f};
            if (gn::raycastObject(from, to, h)) e = h.entity;
        }
        std::string note = "nothing to lift (not a physics entity)";
        if (e) {
            note = "server: " + lift(e, g_lifted);
            void* other = gn::entityOtherHalf(e);
            if (other) g_clientLift = other;
            else note += "; no client half found";
        }
        gn::log("lift: %s %p: %s", e ? gn::Type::of(e).name() : "-", e, note.c_str());
        std::lock_guard<std::mutex> lock(g_m);
        g_liftNote = note;
    }
    Looked l;
    gn::ui::View v;
    gn::ObjectHit h{};
    if (g_look && gn::ui::view(v)) {
        const gn::Vec3 from{v.position[0], v.position[1], v.position[2]};
        const gn::Vec3 to{from.x + v.forward[0] * 200.f, from.y + v.forward[1] * 200.f, from.z + v.forward[2] * 200.f};
        if (gn::raycastObject(from, to, h)) {
            l.hit = true;
            l.at = {h.hit.position[0], h.hit.position[1], h.hit.position[2]};
            l.body = h.body, l.part = h.part;
            const gn::Type type = h.entity ? gn::Type::of(h.entity) : gn::Type();
            l.what = type ? type.name() : "level geometry (no entity)";
            if (h.entity)
                for (uint32_t b : gn::entityBodies(h.entity)) {
                    gn_transform bt{};
                    if (gn::bodyTransform(h.entity, b, bt)) l.bodies.push_back({bt.trans[0], bt.trans[1], bt.trans[2]});
                }
        }
    }
    std::lock_guard<std::mutex> lock(g_m);
    g_looked = std::move(l);
}

void clientTick(float dt) {
    if (void* e = g_clientLift.exchange(nullptr)) {
        const std::string note = "client: " + lift(e, g_liftedClient);
        gn::log("lift: %s %p: %s", gn::Type::of(e).name(), e, note.c_str());
        std::lock_guard<std::mutex> lock(g_m);
        g_clientNote = note;
    }
    if (g_clientPutBack.exchange(false)) {
        const std::string note = "client: " + putBack(g_liftedClient);
        gn::log("%s", note.c_str());
        std::lock_guard<std::mutex> lock(g_m);
        g_clientNote = note;
    }
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
        if (g_look && g_looked.hit) {
            const Looked& l = g_looked;
            gn::ui::marker(v, l.at, IM_COL32(255, 80, 200, 255), 5.f);
            char label[200];
            snprintf(label, sizeof(label), "%s (body %08x, part %08x, %d bodies)", l.what.c_str(), l.body, l.part, int(l.bodies.size()));
            gn::ui::text3d(v, {l.at.x, l.at.y + 0.3f, l.at.z}, label, IM_COL32(255, 160, 230, 255));
            for (const gn::Vec3& b : l.bodies) {
                gn::ui::box3d(v, {b.x - 0.15f, b.y - 0.15f, b.z - 0.15f}, {b.x + 0.15f, b.y + 0.15f, b.z + 0.15f}, IM_COL32(255, 80, 200, 220));
                gn::ui::line3d(v, b, l.at, IM_COL32(255, 80, 200, 120));
            }
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
        b = g_look;
        if (ImGui::Checkbox("What the camera looks at", &b)) g_look = b, gn::config::setBool("look", b);
        if (ImGui::Button("Lift its collision 1 m")) g_liftWanted = 1;
        ImGui::SameLine();
        if (ImGui::Button("Put it back")) g_liftWanted = -1;
        {
            std::lock_guard<std::mutex> lock(g_m);
            if (!g_liftNote.empty()) ImGui::TextDisabled("%s", g_liftNote.c_str());
            if (!g_clientNote.empty()) ImGui::TextDisabled("%s", g_clientNote.c_str());
        }
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
    g_look = gn::config::getBool("look", true);
    if (!gn::ui::onDraw(draw)) {
        gn::log("no overlay: nothing to draw with");
        return false;
    }
    gn::onClientTick(clientTick);
    gn::onServerTick(serverTick);
    return true;
}

// Reloadable (GridNode menu, Mods: Reload, or when the DLL changes): nothing of its own to stop;
// GridNode takes out its callbacks. Bodies it lifted stay where they are (Put it back first).
GN_MOD_UNLOAD() { return true; }
