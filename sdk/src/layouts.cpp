// Layouts (M7): changes to a level's objects that GridNode applies at runtime, and the overlay
// editor that makes them.
//
// A layout file (layouts/<level>.yaml next to gridnode.dll) lists edits. An edit names an object of
// the level by its class without the realm ("StaticModelEntity", "PamStaticModelEntity") and the
// position it is created at, and gives the transform it should have. Static models exist twice, a
// server and a client entity at the same place, and both are moved.
//
// Edits are applied as entities are created (GridNode listens to their creation, so objects that
// stream in later are covered too): the creation info carries the transform the level gives them;
// a match is moved at the next tick of its realm (SpatialEntity_setTransform, as the game moves
// things). Objects created before the layout was loaded are matched from the tracked entities.

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"
#include "imgui.h"

namespace gn::host::layouts {
namespace {

namespace e = gn::engine;

// ---- the layout ---------------------------------------------------------------------------------------
struct Edit {
    std::string kind;   // class name without Server / Client
    float at[3] = {};   // where the level creates it
    gn_transform to{};  // where it should be
};

std::mutex g_m;
std::string g_level;            // the level the edits are for (g_m)
std::vector<Edit> g_edits;      // (g_m)
bool g_dirty = false;           // unsaved changes (g_m)
std::wstring g_dir;             // layouts folder

std::atomic<bool> g_sweptThisLevel{false}; // existing objects matched since the level started
constexpr float kMatch = 0.05f; // metres: how close a created object must be to an edit's position

std::string kindOf(const std::string& cls) {
    std::string k = cls;
    for (const char* realm : {"Server", "Client"}) {
        const size_t at = k.find(realm);
        if (at != std::string::npos && (at == 0 || (at == 3 && k.rfind("Pam", 0) == 0))) {
            k.erase(at, strlen(realm));
            break;
        }
    }
    return k;
}

bool isServerClass(const std::string& cls) { return cls.rfind("Server", 0) == 0 || cls.rfind("PamServer", 0) == 0; }

float dist2(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

std::wstring fileFor(const std::string& level) {
    std::wstring name;
    for (char c : level) name += (c == '/' || c == '\\' || c == ':') ? L'_' : static_cast<wchar_t>(c);
    return g_dir + name + L".yaml";
}

// ---- moves, done on the right thread ----------------------------------------------------------------
struct Move {
    uintptr_t entity;
    const reflect::Type* type;
    gn_transform to;
};
std::mutex g_qm;
std::deque<Move> g_serverQ, g_clientQ; // (g_qm)

void queueMove(uintptr_t entity, const reflect::Type* type, const gn_transform& to) {
    std::lock_guard<std::mutex> lock(g_qm);
    (isServerClass(type->name) ? g_serverQ : g_clientQ).push_back({entity, type, to});
}

// A physics entity's collision follows its transform only when the entity's flags say so: the
// transform-changed notification (flag NotifyTransform) reaches the physics entity, which moves its
// physics object only with flag TeleportPhysics (set at creation from the data's
// TeleportPhysicsOnTransformChanges, which is off for level props). Both are set on the objects
// edits move, and only on those.
void letPhysicsFollow(uintptr_t entity, const reflect::Type* t) {
    namespace c = e::constant;
    static const reflect::Type* server = registry().find("ServerPhysicsEntity");
    static const reflect::Type* client = registry().find("ClientPhysicsEntity");
    if (!t || !((server && registry().isKindOf(t, server)) || (client && registry().isKindOf(t, client)))) return;
    uint32_t flags = 0;
    if (!mem::read(entity + c::Entity_Flags, flags)) return;
    const uint32_t want = flags | c::EntityFlag_NotifyTransform | c::EntityFlag_TeleportPhysics;
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 6)
        GN_LOG("layouts: %s %llx: flags %08x -> %08x, physics object %llx", t->name.c_str(), static_cast<unsigned long long>(entity), flags,
               want, static_cast<unsigned long long>(mem::readOr<uintptr_t>(entity + c::PhysicsEntity_Physics)));
    if (want != flags) mem::write<uint32_t>(entity + c::Entity_Flags, want);
}

void runQueue(std::deque<Move>& q) {
    std::deque<Move> work;
    {
        std::lock_guard<std::mutex> lock(g_qm);
        work.swap(q);
    }
    for (const Move& m : work) {
        // Still the same object (not destroyed and reused since)?
        if (registry().typeOf(mem::ProcessMemory(), m.entity) != m.type) continue;
        letPhysicsFollow(m.entity, m.type);
        entitySetTransform(reinterpret_cast<void*>(m.entity), &m.to);
    }
}

// The edit an object created at `at` falls under, or null (g_m held).
const Edit* editFor(const std::string& cls, const float* at) {
    const std::string k = kindOf(cls);
    for (const Edit& ed : g_edits)
        if (ed.kind == k && dist2(ed.at, at) <= kMatch * kMatch) return &ed;
    return nullptr;
}

// Creation of a static model (any realm): match against the layout by its creation transform.
void onCreatedModel(void* entity, void* ci, void*) {
    if (!entity || !ci) return;
    const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), reinterpret_cast<uintptr_t>(entity));
    if (!t) return;
    float at[3];
    if (!mem::read(reinterpret_cast<uintptr_t>(ci) + e::st::EntityCreationInfo::off::transform + 0x30, at, sizeof(at))) return;
    static std::atomic<int> diag{0};
    if (diag.fetch_add(1) < 4) {
        gn_transform now{};
        const bool ok = entityTransform(entity, now);
        GN_LOG("layouts: created %s: creation info at (%.2f, %.2f, %.2f), entity %s (%.2f, %.2f, %.2f)", t->name.c_str(), at[0], at[1],
               at[2], ok ? "at" : "unreadable", now.trans[0], now.trans[1], now.trans[2]);
    }
    std::lock_guard<std::mutex> lock(g_m);
    if (g_edits.empty() || g_level != levelName()) return;
    if (const Edit* ed = editFor(t->name, at)) queueMove(reinterpret_cast<uintptr_t>(entity), t, ed->to);
}

// Objects that already exist when a layout is loaded: matched by where they are now.
void applyToExisting() {
    std::vector<Edit> edits;
    {
        std::lock_guard<std::mutex> lock(g_m);
        edits = g_edits;
    }
    if (edits.empty()) return;
    int n = 0;
    for (const auto& [p, t] : inspector::tracked()) {
        if (!t) continue;
        gn_transform now{};
        if (!entityTransform(reinterpret_cast<void*>(p), now)) continue;
        const std::string k = kindOf(t->name);
        for (const Edit& ed : edits)
            if (ed.kind == k && dist2(ed.at, now.trans) <= kMatch * kMatch) {
                queueMove(p, t, ed.to);
                ++n;
                break;
            }
    }
    GN_LOG("layouts: %d existing objects moved by the layout", n);
}

// ---- file ---------------------------------------------------------------------------------------------
std::string fmtVec(const float* v) {
    char b[96];
    snprintf(b, sizeof(b), "[%.4f, %.4f, %.4f]", v[0], v[1], v[2]);
    return b;
}

bool save() {
    std::string level;
    std::vector<Edit> edits;
    {
        std::lock_guard<std::mutex> lock(g_m);
        level = g_level;
        edits = g_edits;
    }
    if (level.empty()) return false;
    CreateDirectoryW(g_dir.c_str(), nullptr);
    std::string o = "# GridNode layout: changes GridNode applies to this level when it loads.\n";
    o += "level: " + level + "\nedits:\n";
    for (const Edit& ed : edits) {
        o += "  - kind: " + ed.kind + "\n";
        o += "    at: " + fmtVec(ed.at) + "\n";
        o += "    right: " + fmtVec(ed.to.right) + "\n";
        o += "    up: " + fmtVec(ed.to.up) + "\n";
        o += "    forward: " + fmtVec(ed.to.forward) + "\n";
        o += "    position: " + fmtVec(ed.to.trans) + "\n";
    }
    const std::wstring file = fileFor(level);
    FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f) {
        GN_LOG("layouts: cannot write the layout for %s", level.c_str());
        return false;
    }
    fwrite(o.data(), 1, o.size(), f);
    fclose(f);
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_dirty = false;
    }
    GN_LOG("layouts: saved %llu edits for %s", static_cast<unsigned long long>(edits.size()), level.c_str());
    return true;
}

bool parseVec(const std::string& s, float* v) {
    return sscanf(s.c_str(), " [ %f , %f , %f ]", &v[0], &v[1], &v[2]) == 3;
}

// The level's layout file, if there is one (strict: an edit with a bad line is skipped and logged).
std::vector<Edit> load(const std::string& level) {
    std::vector<Edit> out;
    FILE* f = _wfopen(fileFor(level).c_str(), L"rb");
    if (!f) return out;
    char line[512];
    Edit cur;
    int fields = 0, lineNo = 0;
    auto flush = [&] {
        if (fields == 6) out.push_back(cur);
        else if (fields) GN_LOG("layouts: %s: an edit before line %d is incomplete; skipped", level.c_str(), lineNo);
        cur = Edit{};
        fields = 0;
    };
    while (fgets(line, sizeof(line), f)) {
        ++lineNo;
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        const size_t colon = s.find(':');
        if (s.empty() || s[0] == '#' || colon == std::string::npos) continue;
        std::string key = s.substr(0, colon), val = s.substr(colon + 1);
        const bool item = key.find("- ") != std::string::npos;
        while (!key.empty() && (key[0] == ' ' || key[0] == '-')) key.erase(0, 1);
        while (!val.empty() && val[0] == ' ') val.erase(0, 1);
        if (item) flush();
        bool ok = true;
        if (key == "kind") cur.kind = val, ok = !val.empty();
        else if (key == "at") ok = parseVec(val, cur.at);
        else if (key == "right") ok = parseVec(val, cur.to.right);
        else if (key == "up") ok = parseVec(val, cur.to.up);
        else if (key == "forward") ok = parseVec(val, cur.to.forward);
        else if (key == "position") ok = parseVec(val, cur.to.trans);
        else continue; // level:, edits:
        if (!ok) GN_LOG("layouts: %s line %d: cannot read %s", level.c_str(), lineNo, key.c_str());
        else ++fields;
    }
    flush();
    fclose(f);
    return out;
}

// ---- the editor (render thread; game work is posted to the realms) -------------------------------------
struct Selected {
    std::string kind;
    float at[3] = {};      // where the level created it (the edit's key)
    gn_transform now{};    // where it is
    std::vector<std::pair<uintptr_t, const reflect::Type*>> entities; // its server and client entity
};
std::mutex g_selM;
Selected g_sel;            // (g_selM)
bool g_haveSel = false;    // (g_selM)
std::string g_status;      // (g_selM)

void setStatus(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_selM);
    g_status = s;
}

// Every entity of the same kind at the same place (the server and client halves of one object).
void collectPair(Selected& s, const float* pos) {
    s.entities.clear();
    for (const auto& [p, t] : inspector::tracked()) {
        if (!t || kindOf(t->name) != s.kind) continue;
        gn_transform at{};
        if (entityTransform(reinterpret_cast<void*>(p), at) && dist2(at.trans, pos) <= kMatch * kMatch) s.entities.push_back({p, t});
    }
}

// The entity whose collision a ray hit: a physics entity holds its physics object at
// PhysicsEntity_Physics; failing that, an entity the object points back to.
std::pair<uintptr_t, const reflect::Type*> entityOfPhysics(uintptr_t object) {
    if (!object) return {0, nullptr};
    static const reflect::Type* physicsEntity = registry().find("ServerPhysicsEntity");
    const auto tracked = inspector::tracked();
    for (const auto& [p, t] : tracked)
        if (t && physicsEntity && registry().isKindOf(t, physicsEntity) && mem::readOr<uintptr_t>(p + e::constant::PhysicsEntity_Physics) == object)
            return {p, t};
    for (uint32_t off = 0; off < 0x100; off += 8) {
        const uintptr_t q = mem::readOr<uintptr_t>(object + off);
        if (!q) continue;
        for (const auto& [p, t] : tracked)
            if (p == q && t && isServerClass(t->name) && t->name.find("Character") == std::string::npos) return {p, t};
    }
    return {0, nullptr};
}

// Server thread: a ray from the camera along its view; the object whose collision it hits.
void pick() {
    server().post([] {
        gn_transform cam{};
        gn_movement mv{};
        const bool haveMove = movementGet(&mv) == GN_OK;
        float from[3], dir[3];
        if (cameraTransform(cam)) {
            memcpy(from, cam.trans, sizeof(from));
            memcpy(dir, cam.forward, sizeof(dir));
            // Use the character camera's sense of "forward" if the view's row points the other way.
            if (haveMove && dir[0] * mv.camera_forward[0] + dir[1] * mv.camera_forward[1] + dir[2] * mv.camera_forward[2] < 0.f)
                for (float& v : dir) v = -v;
        } else {
            void* faith = localServerCharacter();
            gn_transform ft{};
            if (!faith || !entityTransform(faith, ft) || !haveMove) return setStatus("Pick: no camera");
            from[0] = ft.trans[0], from[1] = ft.trans[1] + 1.6f, from[2] = ft.trans[2];
            memcpy(dir, mv.camera_forward, sizeof(dir));
        }
        const float to[3] = {from[0] + dir[0] * 200.f, from[1] + dir[1] * 200.f, from[2] + dir[2] * 200.f};
        gn_ray_hit hit{};
        uintptr_t object = 0;
        uint32_t body = 0;
        const int r = raycastObject(from, to, &hit, &object, &body);
        if (r == GN_ERR_NOT_FOUND) return setStatus("Pick: nothing in front of the camera (200 m)");
        if (r != GN_OK) return setStatus("Pick: ray casts are unavailable");
        auto [entity, type] = entityOfPhysics(object);
        static std::atomic<int> diag{0};
        if (diag.fetch_add(1) < 6) {
            const reflect::Type* ot = object ? registry().typeOf(mem::ProcessMemory(), object) : nullptr;
            GN_LOG("layouts: pick hit body %08x at %.1f m: physics object %llx (vtable %llx, %s) -> %s %llx", body, hit.distance,
                   static_cast<unsigned long long>(object), static_cast<unsigned long long>(object ? mem::readOr<uintptr_t>(object) : 0),
                   ot ? ot->name.c_str() : "no reflected type", type ? type->name.c_str() : "no entity",
                   static_cast<unsigned long long>(entity));
        }
        bool guessed = false;
        if (!type) { // no physics entity owns what was hit: the nearest object to the hit, said so
            static const reflect::Type* spatial = registry().find("SpatialEntity");
            float best = 3.f * 3.f;
            for (const auto& [p, t] : inspector::tracked()) {
                if (!t || !spatial || !registry().isKindOf(t, spatial) || !isServerClass(t->name)) continue;
                if (t->name.find("Character") != std::string::npos || t->name.find("VisualEnvironment") != std::string::npos) continue;
                gn_transform at{};
                if (!entityTransform(reinterpret_cast<void*>(p), at)) continue;
                const float d = dist2(at.trans, hit.position);
                if (d < best) best = d, entity = p, type = t, guessed = true;
            }
        }
        Selected s;
        if (!type || !entityTransform(reinterpret_cast<void*>(entity), s.now)) {
            char b[160];
            snprintf(b, sizeof(b), "Pick: hit something %.1f m away that is not a movable object (part of the level's static geometry?)",
                     hit.distance);
            return setStatus(b);
        }
        s.kind = kindOf(type->name);
        // The key is where the level put it: an existing edit's position if this one was moved.
        memcpy(s.at, s.now.trans, sizeof(s.at));
        {
            std::lock_guard<std::mutex> lock(g_m);
            for (const Edit& ed : g_edits)
                if (ed.kind == s.kind && dist2(ed.to.trans, s.now.trans) <= kMatch * kMatch) memcpy(s.at, ed.at, sizeof(s.at));
        }
        collectPair(s, s.now.trans);
        char b[160];
        snprintf(b, sizeof(b), "Picked %s (%llu entities), hit %.1f m away%s", s.kind.c_str(),
                 static_cast<unsigned long long>(s.entities.size()), hit.distance,
                 guessed ? " (a guess: the nearest object to the hit)" : "");
        std::lock_guard<std::mutex> lock(g_selM);
        g_sel = s;
        g_haveSel = true;
        g_status = b;
    });
}

// Moves the selection to `to` (both realms) and records the edit.
void moveSelection(const gn_transform& to) {
    Selected s;
    {
        std::lock_guard<std::mutex> lock(g_selM);
        if (!g_haveSel) return;
        g_sel.now = to;
        s = g_sel;
    }
    for (const auto& [p, t] : s.entities) queueMove(p, t, to);
    std::lock_guard<std::mutex> lock(g_m);
    for (Edit& ed : g_edits)
        if (ed.kind == s.kind && dist2(ed.at, s.at) <= kMatch * kMatch) {
            ed.to = to;
            g_dirty = true;
            return;
        }
    Edit ed;
    ed.kind = s.kind;
    memcpy(ed.at, s.at, sizeof(ed.at));
    ed.to = to;
    g_edits.push_back(ed);
    g_dirty = true;
}

void resetSelection() {
    Selected s;
    {
        std::lock_guard<std::mutex> lock(g_selM);
        if (!g_haveSel) return;
        s = g_sel;
    }
    gn_transform back = s.now;
    memcpy(back.trans, s.at, sizeof(s.at));
    // The level's own rotation is not stored: only position edits can be fully undone here.
    for (const auto& [p, t] : s.entities) queueMove(p, t, back);
    {
        std::lock_guard<std::mutex> lock(g_selM);
        g_sel.now = back;
    }
    std::lock_guard<std::mutex> lock(g_m);
    for (size_t i = 0; i < g_edits.size(); ++i)
        if (g_edits[i].kind == s.kind && dist2(g_edits[i].at, s.at) <= kMatch * kMatch) {
            g_edits.erase(g_edits.begin() + static_cast<long>(i));
            g_dirty = true;
            break;
        }
}

void yaw(gn_transform& t, float degrees) {
    const float a = degrees * 3.14159265f / 180.f, c = std::cos(a), s = std::sin(a);
    auto rot = [&](float* v) {
        const float x = v[0] * c + v[2] * s, z = -v[0] * s + v[2] * c;
        v[0] = x, v[2] = z;
    };
    rot(t.right);
    rot(t.forward);
}

} // namespace

// ---- host interface ----------------------------------------------------------------------------------
void start(const std::wstring& dir) {
    g_dir = dir + L"layouts\\";
    for (const char* cls : {"ServerStaticModelEntity", "ClientStaticModelEntity"})
        onCreated("gridnode", cls, &onCreatedModel, nullptr); // Pam* models derive from these
}

// GridNode's thread, when a level starts: its layout, if any.
void onLevel(const std::string& level) {
    std::vector<Edit> edits = load(level);
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_level = level;
        g_edits = edits;
        g_dirty = false;
    }
    {
        std::lock_guard<std::mutex> lock(g_selM);
        g_haveSel = false;
    }
    g_sweptThisLevel = false;
    if (!edits.empty()) GN_LOG("layouts: %llu edits for %s", static_cast<unsigned long long>(edits.size()), level.c_str());
}

void serverTick() { runQueue(g_serverQ); }

void clientTick() {
    runQueue(g_clientQ);
    // Once Faith is in the level, catch objects created before the layout was read.
    if (!g_sweptThisLevel.load() && localClientCharacter()) {
        g_sweptThisLevel = true;
        bool any;
        {
            std::lock_guard<std::mutex> lock(g_m);
            any = !g_edits.empty();
        }
        if (any) std::thread([] { applyToExisting(); }).detach();
    }
}

void draw(bool* open) {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Layout editor", open)) {
        ImGui::End();
        return;
    }
    size_t count;
    bool dirty;
    std::string level;
    {
        std::lock_guard<std::mutex> lock(g_m);
        count = g_edits.size();
        dirty = g_dirty;
        level = g_level;
    }
    ImGui::TextDisabled("%s", level.empty() ? "no level" : level.c_str());
    ImGui::Text("%llu edit%s%s", static_cast<unsigned long long>(count), count == 1 ? "" : "s", dirty ? " (not saved)" : "");
    if (ImGui::Button("Save")) save();
    ImGui::SameLine();
    if (ImGui::Button("Reload from file")) {
        onLevel(level);
        std::thread([] { applyToExisting(); }).detach();
    }
    ImGui::SeparatorText("Object");
    if (ImGui::Button("Pick what the camera points at")) pick();
    Selected s;
    bool have;
    std::string status;
    {
        std::lock_guard<std::mutex> lock(g_selM);
        s = g_sel;
        have = g_haveSel;
        status = g_status;
    }
    if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());
    if (have) {
        ImGui::Text("%s  (%llu entities)", s.kind.c_str(), static_cast<unsigned long long>(s.entities.size()));
        gn_transform t = s.now;
        bool changed = ImGui::DragFloat3("Position", t.trans, 0.05f, 0.f, 0.f, "%.2f");
        static float step = 15.f;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::InputFloat("##step", &step, 0.f, 0.f, "%.0f deg");
        ImGui::SameLine();
        if (ImGui::Button("Turn left")) yaw(t, step), changed = true;
        ImGui::SameLine();
        if (ImGui::Button("Turn right")) yaw(t, -step), changed = true;
        if (changed) moveSelection(t);
        if (ImGui::Button("Put back where the level had it")) resetSelection();
        ImGui::TextDisabled("level position (%.2f, %.2f, %.2f)", s.at[0], s.at[1], s.at[2]);
    } else {
        ImGui::TextDisabled("Point the camera at an object and press Pick.");
    }
    ImGui::End();
}

} // namespace gn::host::layouts
