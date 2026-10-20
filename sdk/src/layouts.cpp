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
    gn_transform from{}; // the level's own placement (rotation included), for putting it back
    bool haveFrom = false; // false for layout files written before `from` was saved
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

// ---- collision bodies ----------------------------------------------------------------------------------
// A physics entity's component lists its bodies by id; an id leads to its RigidBody through the
// physics manager's owner table (bits 16..23) and that owner's object table (low 16 bits). Moving an
// entity does not move its bodies (the component only refreshes their bounds), so edits move them
// with RigidBody's own setTransform.
template <class F>
void forEachBody(uintptr_t entity, F&& fn) {
    namespace c = e::constant;
    static const reflect::Type* rigid = registry().find("RigidBody");
    const uintptr_t comp = mem::readOr<uintptr_t>(entity + c::PhysicsEntity_Physics);
    if (!comp || !rigid) return;
    const uint16_t count = mem::readOr<uint16_t>(comp + c::PhysicsComponent_BodyCount);
    const uintptr_t ids = mem::readOr<uintptr_t>(comp + c::PhysicsComponent_BodyIds);
    const uintptr_t manager = mem::readOr<uintptr_t>(comp + c::PhysicsComponent_Manager);
    const uintptr_t owners = manager ? mem::readOr<uintptr_t>(manager + c::PhysicsManager_Owners) : 0;
    if (!ids || !owners || count > 64) return;
    for (uint16_t i = 0; i < count; ++i) {
        const uint32_t id = mem::readOr<uint32_t>(ids + i * 4u, 0xffffffffu);
        if (id == 0xffffffffu) continue;
        const uintptr_t owner = mem::readOr<uintptr_t>(owners + ((id >> 16) & 0xff) * 8);
        const uintptr_t table = owner ? mem::readOr<uintptr_t>(owner + c::PhysicsOwner_Objects) : 0;
        const uintptr_t body = table ? mem::readOr<uintptr_t>(table + (id & 0xffff) * 8) : 0;
        const reflect::Type* bt = body ? registry().typeOf(mem::ProcessMemory(), body) : nullptr;
        fn(id, bt && registry().isKindOf(bt, rigid) ? body : 0);
    }
}

// `p` (a point or a direction) from `from`'s frame into `to`'s. Rows may be scaled, so the local
// coordinates are dot / |row|^2.
void reframe(const gn_transform& from, const gn_transform& to, const float* p, bool point, float* out) {
    float v[3] = {p[0], p[1], p[2]};
    if (point)
        for (int k = 0; k < 3; ++k) v[k] -= from.trans[k];
    const float* rows[3] = {from.right, from.up, from.forward};
    const float* rowsTo[3] = {to.right, to.up, to.forward};
    float l[3];
    for (int r = 0; r < 3; ++r) {
        const float* b = rows[r];
        const float n = b[0] * b[0] + b[1] * b[1] + b[2] * b[2];
        l[r] = n > 1e-12f ? (v[0] * b[0] + v[1] * b[1] + v[2] * b[2]) / n : 0.f;
    }
    for (int k = 0; k < 3; ++k) out[k] = l[0] * rowsTo[0][k] + l[1] * rowsTo[1][k] + l[2] * rowsTo[2][k] + (point ? to.trans[k] : 0.f);
}

void normalize(float* v) {
    const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 1e-6f)
        for (int k = 0; k < 3; ++k) v[k] /= n;
}

// Moves the bodies of `entity` from `before` to `after`, each keeping its place relative to the
// entity. On the entity's realm thread.
void moveBodies(uintptr_t entity, const reflect::Type* t, const gn_transform& before, const gn_transform& after) {
    namespace c = e::constant;
    int moved = 0, total = 0;
    forEachBody(entity, [&](uint32_t, uintptr_t body) {
        ++total;
        if (!body) return;
        const uintptr_t vt = mem::readOr<uintptr_t>(body);
        const uintptr_t get = vt ? mem::readOr<uintptr_t>(vt + c::RigidBody_vf_getTransform) : 0;
        const uintptr_t set = vt ? mem::readOr<uintptr_t>(vt + c::RigidBody_vf_setTransform) : 0;
        if (!mem::inImage(get) || !mem::inImage(set)) return;
        alignas(16) gn_transform was{}, now{};
        reinterpret_cast<void* (*)(uintptr_t, gn_transform*)>(get)(body, &was);
        if (!std::isfinite(was.trans[0]) || !std::isfinite(was.trans[1]) || !std::isfinite(was.trans[2])) return;
        reframe(before, after, was.right, false, now.right);
        reframe(before, after, was.up, false, now.up);
        reframe(before, after, was.forward, false, now.forward);
        reframe(before, after, was.trans, true, now.trans);
        normalize(now.right), normalize(now.up), normalize(now.forward); // bodies carry no scale
        reinterpret_cast<void (*)(uintptr_t, const gn_transform*, bool)>(set)(body, &now, true);
        ++moved;
    });
    static std::atomic<int> said{0};
    if (moved != total || said.fetch_add(1) < 40)
        GN_LOG("layouts: %s %llx: moved %d of %d collision bodies", t->name.c_str(), static_cast<unsigned long long>(entity), moved, total);
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
        gn_transform before{};
        const bool had = entityTransform(reinterpret_cast<void*>(m.entity), before);
        entitySetTransform(reinterpret_cast<void*>(m.entity), &m.to);
        if (had) moveBodies(m.entity, m.type, before, m.to);
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
        if (ed.haveFrom) {
            o += "    original_right: " + fmtVec(ed.from.right) + "\n";
            o += "    original_up: " + fmtVec(ed.from.up) + "\n";
            o += "    original_forward: " + fmtVec(ed.from.forward) + "\n";
        }
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
    int fields = 0, origFields = 0, lineNo = 0;
    auto flush = [&] {
        if (fields == 6) {
            cur.haveFrom = origFields == 3;
            memcpy(cur.from.trans, cur.at, sizeof(cur.at));
            out.push_back(cur);
        }
        else if (fields) GN_LOG("layouts: %s: an edit before line %d is incomplete; skipped", level.c_str(), lineNo);
        cur = Edit{};
        fields = 0;
        origFields = 0;
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
        else if (key == "original_right" || key == "original_up" || key == "original_forward") {
            float* row = key == "original_right" ? cur.from.right : key == "original_up" ? cur.from.up : cur.from.forward;
            if (parseVec(val, row)) ++origFields;
            else GN_LOG("layouts: %s line %d: cannot read %s", level.c_str(), lineNo, key.c_str());
            continue;
        }
        else continue; // level:, edits:
        if (!ok) GN_LOG("layouts: %s line %d: cannot read %s", level.c_str(), lineNo, key.c_str());
        else ++fields;
    }
    flush();
    fclose(f);
    return out;
}

// ---- props inside static model groups (experimental) -------------------------------------------------
// A group is one entity per realm at the world origin with one GroupRigidBody: a Havok compound shape
// with one instance per copy of each member. A ray hit's part names the copy; the body maps it to
// its instance. The client group re-places its render objects from the instances when told its
// transform changed (InstanceTransforms in the member data take precedence when a member has them).
struct GroupProp {
    uintptr_t serverGroup = 0, clientGroup = 0;
    uintptr_t serverBody = 0, clientBody = 0; // GroupRigidBody
    uint32_t part = 0;
    uintptr_t member = 0;                     // StaticModelGroupMemberData (client block's)
    uintptr_t block = 0;                      // the client group's render block for the member
    uint32_t copy = 0;                        // index of this copy within the member
};

bool finite(const float* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

// A reflected field's offset in StaticModelGroupMemberData (from the game's own type info).
uint32_t memberField(const char* name) {
    static const reflect::Type* t = registry().find("StaticModelGroupMemberData");
    reflect::FieldInfo fi;
    return t && registry().findField(t, name, fi) ? fi.field->offset : 0xffffffffu;
}

uintptr_t dataOf(uintptr_t entity, const char* typeName) {
    const reflect::Type* want = registry().find(typeName);
    if (!want) return 0;
    for (uint32_t off = 0x18; off <= 0x60; off += 8) {
        const uintptr_t d = mem::readOr<uintptr_t>(entity + off);
        const reflect::Type* t = d ? registry().typeOf(mem::ProcessMemory(), d) : nullptr;
        if (t && registry().isKindOf(t, want)) return d;
    }
    return 0;
}

// The group body behind an entity's physics component (any body type).
uintptr_t groupBodyOf(uintptr_t entity) {
    namespace c = e::constant;
    static const reflect::Type* groupBody = registry().find("GroupRigidBody");
    const uintptr_t comp = mem::readOr<uintptr_t>(entity + c::PhysicsEntity_Physics);
    if (!comp || !groupBody) return 0;
    const uint16_t count = mem::readOr<uint16_t>(comp + c::PhysicsComponent_BodyCount);
    const uintptr_t ids = mem::readOr<uintptr_t>(comp + c::PhysicsComponent_BodyIds);
    const uintptr_t manager = mem::readOr<uintptr_t>(comp + c::PhysicsComponent_Manager);
    const uintptr_t owners = manager ? mem::readOr<uintptr_t>(manager + c::PhysicsManager_Owners) : 0;
    if (!ids || !owners || count > 64) return 0;
    for (uint16_t i = 0; i < count; ++i) {
        const uint32_t id = mem::readOr<uint32_t>(ids + i * 4u, 0xffffffffu);
        const uintptr_t owner = id != 0xffffffffu ? mem::readOr<uintptr_t>(owners + ((id >> 16) & 0xff) * 8) : 0;
        const uintptr_t table = owner ? mem::readOr<uintptr_t>(owner + c::PhysicsOwner_Objects) : 0;
        const uintptr_t body = table ? mem::readOr<uintptr_t>(table + (id & 0xffff) * 8) : 0;
        const reflect::Type* bt = body ? registry().typeOf(mem::ProcessMemory(), body) : nullptr;
        if (bt && registry().isKindOf(bt, groupBody)) return body;
    }
    return 0;
}

// The compound instance of `part` in a group body (0 if unknown).
uintptr_t groupInstance(uintptr_t body, uint32_t part, int* indexOut = nullptr) {
    namespace c = e::constant;
    const uintptr_t shape = body ? mem::readOr<uintptr_t>(body + c::GroupRigidBody_Shape) : 0;
    const uintptr_t map = body ? mem::readOr<uintptr_t>(body + c::GroupRigidBody_PartToInstance) : 0;
    const uintptr_t instances = shape ? mem::readOr<uintptr_t>(shape + c::CompoundShape_Instances) : 0;
    if (!map || !instances || part > 0xffff) return 0;
    const int16_t index = mem::readOr<int16_t>(map + part * 4u, -1);
    if (index < 0) return 0;
    if (indexOut) *indexOut = index;
    return instances + static_cast<uintptr_t>(index) * c::ShapeInstance_Size;
}

// An instance's transform (scale folded into the rows, as the game's 0x143f50740 does).
bool readInstance(uintptr_t inst, gn_transform& out) {
    float m[20];
    if (!inst || !mem::read(inst, m, sizeof(m)) || !finite(m, 3) || !finite(m + 12, 3) || !finite(m + 16, 3)) return false;
    for (int k = 0; k < 3; ++k) {
        out.right[k] = m[k] * m[16];
        out.up[k] = m[4 + k] * m[17];
        out.forward[k] = m[8 + k] * m[18];
        out.trans[k] = m[12 + k];
    }
    return true;
}

// Writes rotation (unit rows), scale and translation; the w lanes (other data) are kept.
void writeInstance(uintptr_t inst, const gn_transform& t) {
    float m[20];
    if (!inst || !mem::read(inst, m, sizeof(m))) return;
    const float* rows[3] = {t.right, t.up, t.forward};
    for (int r = 0; r < 3; ++r) {
        const float n = std::sqrt(rows[r][0] * rows[r][0] + rows[r][1] * rows[r][1] + rows[r][2] * rows[r][2]);
        if (n < 1e-6f) return;
        for (int k = 0; k < 3; ++k) m[r * 4 + k] = rows[r][k] / n;
        m[16 + r] = n;
    }
    for (int k = 0; k < 3; ++k) m[12 + k] = t.trans[k];
    mem::write(inst, m, sizeof(m));
}

// The client block's member and copy index for `part`.
bool memberOf(uintptr_t clientGroup, uint32_t part, uintptr_t& member, uint32_t& copy, uintptr_t& blockOut) {
    namespace c = e::constant;
    static const uint32_t partRange = memberField("PhysicsPartRange"), perCopy = memberField("PhysicsPartCountPerInstance"),
                          copies = memberField("InstanceCount");
    if (partRange == 0xffffffffu || perCopy == 0xffffffffu || copies == 0xffffffffu) return false;
    const uintptr_t blocks = mem::readOr<uintptr_t>(clientGroup + c::ClientStaticModelGroup_Members);
    const int32_t count = mem::readOr<int32_t>(clientGroup + c::ClientStaticModelGroup_MemberCount);
    if (!blocks || count <= 0 || count > 4096) return false;
    for (int32_t b = 0; b < count; ++b) {
        const uintptr_t block = blocks + static_cast<uintptr_t>(b) * c::GroupMemberBlock_Size;
        const uintptr_t m = mem::readOr<uintptr_t>(block + c::GroupMemberBlock_Member);
        if (!m) continue;
        const uint32_t first = mem::readOr<uint32_t>(m + partRange);
        const uint32_t per = mem::readOr<uint32_t>(m + perCopy);
        const uint32_t n = mem::readOr<uint32_t>(m + copies);
        if (per == 0 || part < first || part >= first + per * n) continue;
        member = m;
        copy = (part - first) / per;
        blockOut = block;
        return true;
    }
    return false;
}

// The member's own InstanceTransforms entry for the copy, if the member has them (0 if not).
uintptr_t memberTransform(uintptr_t member, uint32_t copy) {
    static const uint32_t off = memberField("InstanceTransforms");
    const uintptr_t arr = member && off != 0xffffffffu ? mem::readOr<uintptr_t>(member + off) : 0;
    const uint32_t n = arr ? mem::readOr<uint32_t>(arr - 4) : 0;
    return copy < n ? arr + copy * 0x40u : 0;
}

// The render object of the copy (client thread): the block's cached list, or the physics
// component's object for the copy's first part (how 0x14318fbc0 gathers them).
uintptr_t renderObjectOf(const GroupProp& g) {
    static const uint32_t partRange = memberField("PhysicsPartRange"), perCopy = memberField("PhysicsPartCountPerInstance");
    if (!g.block || !g.member) return 0;
    if (const uintptr_t list = mem::readOr<uintptr_t>(g.block + 0xc8)) return mem::readOr<uintptr_t>(list + g.copy * 8u);
    const uintptr_t comp = mem::readOr<uintptr_t>(g.block + 0x20);
    const uintptr_t vt = comp ? mem::readOr<uintptr_t>(comp) : 0;
    const uintptr_t fn = vt ? mem::readOr<uintptr_t>(vt + 0x210) : 0;
    if (!mem::inImage(fn)) return 0;
    const uint32_t part = mem::readOr<uint32_t>(g.member + partRange) + mem::readOr<uint32_t>(g.member + perCopy) * g.copy;
    return reinterpret_cast<uintptr_t (*)(uintptr_t, uint32_t)>(fn)(comp, part);
}

// Client thread: moves only this copy's render object, as 0x14318fbc0 does for each copy: set its
// transform (vtable +0x10), build an update (0x143769330) and hand it to the render world
// (0x1436d6ce0 with *(0x1424a9b98)+0x48).
void placeCopy(const GroupProp& g, const gn_transform& world) {
    const uintptr_t obj = renderObjectOf(g);
    const uintptr_t vt = obj ? mem::readOr<uintptr_t>(obj) : 0;
    const uintptr_t setT = vt ? mem::readOr<uintptr_t>(vt + 0x10) : 0;
    const uintptr_t renderRoot = mem::readOr<uintptr_t>(mem::rva(e::global::RenderRoot));
    const uintptr_t renderWorld = renderRoot ? mem::readOr<uintptr_t>(renderRoot + 0x48) : 0;
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 6)
        GN_LOG("layouts: group part %u: render object %llx (vtable %llx), block %llx, list %llx, render world %llx", g.part,
               static_cast<unsigned long long>(obj), static_cast<unsigned long long>(vt), static_cast<unsigned long long>(g.block),
               static_cast<unsigned long long>(g.block ? mem::readOr<uintptr_t>(g.block + 0xc8) : 0), static_cast<unsigned long long>(renderWorld));
    if (!mem::inImage(setT) || !renderWorld) return;
    static std::atomic<int> diag{0};
    if (diag.fetch_add(1) < 3) {
        // Research: is this the drawn copy? Where it is now, its render handles, and the class
        // check 0x14318fbc0 applies to objects from the component (ids in [*0x142843238, *0x14284323a]).
        float at[16] = {};
        mem::read(obj + 0x10, at, sizeof(at));
        const uintptr_t typeFn = mem::readOr<uintptr_t>(vt);
        const uintptr_t ci = mem::inImage(typeFn) ? reinterpret_cast<uintptr_t (*)(uintptr_t)>(typeFn)(obj) : 0;
        GN_LOG("layouts: group part %u: render object at (%.2f, %.2f, %.2f) [copy at (%.2f, %.2f, %.2f)], radius %.2f, handles +0x78 %08x +0x84 %08x "
               "+0x88 %08x +0x90 %04x +0x98 %04x, class id %u (range %u..%u), mesh %llx",
               g.part, at[12], at[13], at[14], world.trans[0], world.trans[1], world.trans[2], mem::readOr<float>(obj + 0x5c),
               mem::readOr<uint32_t>(obj + 0x78), mem::readOr<uint32_t>(obj + 0x84), mem::readOr<uint32_t>(obj + 0x88),
               mem::readOr<uint16_t>(obj + 0x90), mem::readOr<uint16_t>(obj + 0x98), ci ? mem::readOr<uint16_t>(ci + 0x28) : 0,
               mem::readOr<uint16_t>(mem::rva(0x142843238)), mem::readOr<uint16_t>(mem::rva(0x14284323a)),
               static_cast<unsigned long long>(mem::readOr<uintptr_t>(obj + 0x60)));
        // And what the block's other fields hold (render data for the member).
        for (uint32_t off = 0; off < 0x130; off += 8) {
            const uintptr_t q = mem::readOr<uintptr_t>(g.block + off);
            if (q < 0x10000) continue;
            const reflect::Type* qt = registry().typeOf(mem::ProcessMemory(), q);
            const uintptr_t qvt = mem::readOr<uintptr_t>(q);
            GN_LOG("layouts:   block +%03x %llx %s (vtable %llx)", off, static_cast<unsigned long long>(q), qt ? qt->name.c_str() : "-",
                   static_cast<unsigned long long>(mem::inImage(qvt) ? qvt : 0));
        }
    }
    alignas(16) gn_transform t = world;
    t.right[3] = t.up[3] = t.forward[3] = 0.f, t.trans[3] = 1.f;
    reinterpret_cast<void (*)(uintptr_t, gn_transform*)>(setT)(obj, &t);
    static std::atomic<int> back{0};
    if (back.fetch_add(1) < 12) {
        float now[16] = {};
        mem::read(obj + 0x10, now, sizeof(now));
        GN_LOG("layouts: group part %u: render object now at (%.2f, %.2f, %.2f) right (%.2f, %.2f, %.2f) (asked (%.2f, %.2f, %.2f)); render path %llx",
               g.part, now[12], now[13], now[14], now[0], now[1], now[2], t.trans[0], t.trans[1], t.trans[2],
               static_cast<unsigned long long>(mem::readOr<uintptr_t>(mem::rva(0x142568ce8))));
        client().post([obj, part = g.part] { // and a frame later: kept, or reset by something?
            float later[16] = {};
            mem::read(obj + 0x10, later, sizeof(later));
            GN_LOG("layouts: group part %u: a frame later at (%.2f, %.2f, %.2f) right (%.2f, %.2f, %.2f)", part, later[12], later[13], later[14], later[0],
                   later[1], later[2]);
        });
    }
    alignas(16) uint8_t update[0x100] = {};
    // Set before building, as 0x14318fbc0 does (offsets from its local at rsp-0x148).
    const uint32_t lodScale = mem::readOr<uint32_t>(mem::rva(0x141a4e8dc));
    memcpy(update + 0x60, &lodScale, 4);
    update[0x88] = update[0x89] = 0;
    update[0x8d] = 2;
    update[0x8e] = 0;
    reinterpret_cast<void (*)(uintptr_t, uint8_t*)>(mem::rva(e::function::RenderObject_buildUpdate))(obj, update);
    const uintptr_t objPlus8 = obj + 8;
    memcpy(update + 0x70, &objPlus8, 8);
    const uint32_t tag = mem::readOr<uint32_t>(obj + 0x8c);
    memcpy(update + 0x90, &tag, 4);
    reinterpret_cast<void (*)(uintptr_t, uint8_t*)>(mem::rva(e::function::RenderWorld_submitUpdate))(renderWorld, update);
}

// Client thread: the game's own placement of every copy of the member (what the group's
// transform-changed does per member), from the physics instances or InstanceTransforms.
void replaceMemberByGame(const GroupProp& g) {
    static const uint32_t partRange = memberField("PhysicsPartRange");
    if (!g.block || !g.member || !g.clientGroup) return;
    const uintptr_t space = mem::readOr<uintptr_t>(g.clientGroup + e::cls::ComponentEntity::off::transformSpace);
    uint8_t sel[2] = {};
    if (!space || !mem::read(space + 9, sel, 2)) return;
    const uintptr_t groupTransform = space + 0x10 + (sel[0] + 2u * sel[1]) * 0x20;
    const uint32_t first = mem::readOr<uint32_t>(g.member + partRange);
    GN_LOG("layouts: group part %u: the game's placement for its member (block %llx, first part %u)", g.part, static_cast<unsigned long long>(g.block), first);
    reinterpret_cast<void (*)(uintptr_t, uintptr_t, uint32_t)>(mem::rva(e::function::ClientStaticModelGroup_placeMember))(g.block, groupTransform, first);
}

// Server thread: puts the copy at `world`, then has the client group re-place its render objects.
void moveGroupProp(const GroupProp& g, const gn_transform& world) {
    gn_transform G{};
    if (!entityTransform(reinterpret_cast<void*>(g.serverGroup), G)) return;
    alignas(16) gn_transform local{};
    gn_transform I{};
    I.right[0] = I.up[1] = I.forward[2] = 1.f;
    reframe(G, I, world.right, false, local.right);
    reframe(G, I, world.up, false, local.up);
    reframe(G, I, world.forward, false, local.forward);
    reframe(G, I, world.trans, true, local.trans);
    const uintptr_t si = groupInstance(g.serverBody, g.part);
    const uintptr_t ci = groupInstance(g.clientBody, g.part);
    {
        static std::atomic<int> checked{0};
        float before[20] = {}, after[20] = {};
        gn_transform cur{};
        if (checked.fetch_add(1) < 1 && mem::read(si, before, sizeof(before)) && readInstance(si, cur)) {
            writeInstance(si, cur); // a read-write round trip must not change the instance
            mem::read(si, after, sizeof(after));
            int diff = 0;
            for (int k = 0; k < 20; ++k) diff += std::fabs(before[k] - after[k]) > 1e-5f;
            GN_LOG("layouts: group instance round trip: %d of 20 floats changed (scale %.3f %.3f %.3f, w %.3f %.3f %.3f %.3f)", diff, before[16],
                   before[17], before[18], before[3], before[7], before[11], before[15]);
        }
    }
    writeInstance(si, local);
    if (ci && ci != si) writeInstance(ci, local);
    if (const uintptr_t mt = memberTransform(g.member, g.copy)) {
        alignas(16) gn_transform row = local;
        row.right[3] = row.up[3] = row.forward[3] = 0.f, row.trans[3] = 1.f;
        mem::write(mt, &row, sizeof(row));
    }
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 20)
        GN_LOG("layouts: group part %u -> (%.2f, %.2f, %.2f): server instance %llx, client instance %llx%s", g.part, local.trans[0], local.trans[1],
               local.trans[2], static_cast<unsigned long long>(si), static_cast<unsigned long long>(ci),
               memberTransform(g.member, g.copy) ? ", member transform too" : "");
    const GroupProp gp = g;
    alignas(16) gn_transform w = world;
    client().post([gp, w] { placeCopy(gp, w); });
}

// Server thread, from a pick that hit a group: fills `g` and the copy's world transform.
bool pickGroupProp(uintptr_t serverGroup, uintptr_t body, uint32_t part, GroupProp& g, gn_transform& world) {
    g = GroupProp{};
    g.serverGroup = serverGroup;
    g.serverBody = body;
    g.part = part & 0xffff;
    int index = -1;
    const uintptr_t si = groupInstance(body, g.part, &index);
    gn_transform local{}, G{};
    if (!si || !readInstance(si, local) || !entityTransform(reinterpret_cast<void*>(serverGroup), G)) {
        GN_LOG("layouts: group part %u: no instance (body %llx)", g.part, static_cast<unsigned long long>(body));
        return false;
    }
    gn_transform I{};
    I.right[0] = I.up[1] = I.forward[2] = 1.f;
    reframe(I, G, local.right, false, world.right);
    reframe(I, G, local.up, false, world.up);
    reframe(I, G, local.forward, false, world.forward);
    reframe(I, G, local.trans, true, world.trans);
    // The client half: the client group made from the same data.
    const uintptr_t data = dataOf(serverGroup, "StaticModelGroupEntityData");
    static const reflect::Type* cg = registry().find("ClientStaticModelGroupEntity");
    for (const auto& [p, t] : inspector::tracked())
        if (t && cg && registry().isKindOf(t, cg) && data && dataOf(p, "StaticModelGroupEntityData") == data) {
            g.clientGroup = p;
            break;
        }
    if (g.clientGroup) {
        g.clientBody = groupBodyOf(g.clientGroup);
        memberOf(g.clientGroup, g.part, g.member, g.copy, g.block);
    }
    const uintptr_t shape = mem::readOr<uintptr_t>(body + e::constant::GroupRigidBody_Shape);
    const uintptr_t clientShape = g.clientBody ? mem::readOr<uintptr_t>(g.clientBody + e::constant::GroupRigidBody_Shape) : 0;
    GN_LOG("layouts: group part %u = instance %d at (%.2f, %.2f, %.2f), group at (%.2f, %.2f, %.2f); shape %llx (client %llx, %s), client group %llx, "
           "member %llx copy %u%s",
           g.part, index, world.trans[0], world.trans[1], world.trans[2], G.trans[0], G.trans[1], G.trans[2], static_cast<unsigned long long>(shape),
           static_cast<unsigned long long>(clientShape), shape == clientShape ? "shared" : "separate", static_cast<unsigned long long>(g.clientGroup),
           static_cast<unsigned long long>(g.member), g.copy, memberTransform(g.member, g.copy) ? " (has its own InstanceTransform)" : "");
    return g.clientGroup != 0;
}

// ---- the editor (render thread; game work is posted to the realms) -------------------------------------
struct Selected {
    std::string kind;
    float at[3] = {};      // where the level created it (the edit's key)
    gn_transform now{};    // where it is
    gn_transform orig{};   // where the level had it (rotation included)
    std::vector<std::pair<uintptr_t, const reflect::Type*>> entities; // its server and client entity
    bool group = false;    // a prop inside a static model group (experimental; not saved)
    GroupProp gp;
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

// The entity whose collision a ray hit: the server physics entity that lists the hit body's id.
std::pair<uintptr_t, const reflect::Type*> entityOfBody(uint32_t body) {
    if (body == 0xffffffffu) return {0, nullptr};
    static const reflect::Type* physicsEntity = registry().find("ServerPhysicsEntity");
    for (const auto& [p, t] : inspector::tracked()) {
        if (!t || !physicsEntity || !registry().isKindOf(t, physicsEntity)) continue;
        bool match = false;
        forEachBody(p, [&](uint32_t id, uintptr_t) { match = match || id == body; });
        if (match) return {p, t};
    }
    return {0, nullptr};
}

// Server thread: a ray from the camera along its view; the object whose collision it hits.
void pick() {
    {
        // A new pick replaces the selection, even when it finds nothing to select: otherwise the
        // editor keeps showing (and moving) the previous object under a "not supported" message.
        std::lock_guard<std::mutex> lock(g_selM);
        g_haveSel = false;
        g_status = "Picking...";
    }
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
        uint32_t body = 0, part = 0xffffffffu;
        const int r = raycastObject(from, to, &hit, &object, &body, &part);
        if (r == GN_ERR_NOT_FOUND) return setStatus("Pick: nothing in front of the camera (200 m)");
        if (r != GN_OK) return setStatus("Pick: ray casts are unavailable");
        auto [entity, type] = entityOfBody(body);
        static std::atomic<int> diag{0};
        if (diag.fetch_add(1) < 20) {
            const reflect::Type* ot = object ? registry().typeOf(mem::ProcessMemory(), object) : nullptr;
            GN_LOG("layouts: pick hit body %08x part %08x at %.1f m: physics object %llx (vtable %llx, %s) -> %s %llx", body, part, hit.distance,
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
                if (t->name.find("Character") != std::string::npos || t->name.find("VisualEnvironment") != std::string::npos ||
                    t->name.find("StaticModelGroup") != std::string::npos)
                    continue;
                gn_transform at{};
                if (!entityTransform(reinterpret_cast<void*>(p), at)) continue;
                const float d = dist2(at.trans, hit.position);
                if (d < best) best = d, entity = p, type = t, guessed = true;
            }
        }
        // A static model group is many props sharing one entity (placed at the world origin) and one
        // collision body; moving it moves all of them. Props inside a group are not editable yet.
        if (type && type->name.find("StaticModelGroup") != std::string::npos) {
            Selected s;
            gn_transform world{};
            if (!pickGroupProp(entity, object, part, s.gp, world)) {
                char b[160];
                snprintf(b, sizeof(b), "Pick: a prop in a static model group (part %u) that GridNode cannot edit", part & 0xffffu);
                return setStatus(b);
            }
            s.group = true;
            s.kind = "Group prop";
            s.now = s.orig = world;
            memcpy(s.at, world.trans, sizeof(s.at));
            char b[200];
            snprintf(b, sizeof(b), "Picked a prop inside a static model group (part %u), %.1f m away. Experimental: small moves only, not saved.",
                     s.gp.part, hit.distance);
            std::lock_guard<std::mutex> lock(g_selM);
            g_sel = s;
            g_haveSel = true;
            g_status = b;
            return;
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
        s.orig = s.now;
        {
            std::lock_guard<std::mutex> lock(g_m);
            for (const Edit& ed : g_edits)
                if (ed.kind == s.kind && dist2(ed.to.trans, s.now.trans) <= kMatch * kMatch) {
                    memcpy(s.at, ed.at, sizeof(s.at));
                    if (ed.haveFrom) {
                        s.orig = ed.from;
                    } else { // older file: the level's position, the current rotation
                        s.orig = s.now;
                        memcpy(s.orig.trans, ed.at, sizeof(ed.at));
                    }
                }
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
    if (s.group) {
        const GroupProp gp = s.gp;
        server().post([gp, to] { moveGroupProp(gp, to); });
        return;
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
    ed.from = s.orig;
    ed.haveFrom = true;
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
    const gn_transform back = s.orig;
    if (s.group) {
        const GroupProp gp = s.gp;
        server().post([gp, back] { moveGroupProp(gp, back); });
    }
    for (const auto& [p, t] : s.entities) queueMove(p, t, back);
    GN_LOG("layouts: %s put back at (%.2f, %.2f, %.2f) (%llu entities)", s.kind.c_str(), back.trans[0], back.trans[1], back.trans[2],
           static_cast<unsigned long long>(s.entities.size()));
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
        if (s.group) {
            ImGui::SameLine();
            if (ImGui::Button("Re-place member (game code)")) {
                const GroupProp gp = s.gp;
                client().post([gp] { replaceMemberByGame(gp); });
            }
        }
        ImGui::TextDisabled("level position (%.2f, %.2f, %.2f)", s.at[0], s.at[1], s.at[2]);
    } else {
        ImGui::TextDisabled("Point the camera at an object and press Pick.");
    }
    ImGui::End();
}

} // namespace gn::host::layouts
