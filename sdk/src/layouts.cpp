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

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <algorithm>
#include <cctype>
#include <unordered_map>
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
std::string g_name;             // the loaded / last saved layout's name, empty if none (g_m)
std::vector<Edit> g_previous;   // the edits of the layout unloaded last, to find what they moved (g_m)

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

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string narrowName(const std::wstring& s) {
    std::string o;
    for (wchar_t c : s) o += c < 128 ? static_cast<char>(c) : '_';
    return o;
}

std::wstring levelKey(const std::string& level) {
    std::wstring name;
    for (char c : level) name += (c == '/' || c == '\\' || c == ':') ? L'_' : static_cast<wchar_t>(c);
    return name;
}

// layouts\<level>\<name>.yaml; the single file per level of earlier versions (layouts\<level>.yaml)
// reads as the layout "default".
std::wstring levelDir(const std::string& level) { return g_dir + levelKey(level) + L"\\"; }
std::wstring legacyFile(const std::string& level) { return g_dir + levelKey(level) + L".yaml"; }
std::wstring fileFor(const std::string& level, const std::string& name) {
    const std::wstring f = levelDir(level) + widen(name) + L".yaml";
    if (name == "default" && GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(legacyFile(level).c_str()) != INVALID_FILE_ATTRIBUTES)
        return legacyFile(level);
    return f;
}

bool validName(const std::string& n) {
    if (n.empty() || n.size() > 60) return false;
    for (char c : n)
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ' ' || c == '.')) return false;
    return n[0] != '.' && n[0] != ' ';
}

std::vector<std::string> listLayouts(const std::string& level) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((levelDir(level) + L"*.yaml").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            n.resize(n.size() - 5);
            out.push_back(narrowName(n));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (GetFileAttributesW(legacyFile(level).c_str()) != INVALID_FILE_ATTRIBUTES &&
        std::find(out.begin(), out.end(), "default") == out.end())
        out.push_back("default");
    std::sort(out.begin(), out.end());
    return out;
}

// Small settings: auto-load (layouts\settings.txt) and each level's last used layout (last.txt).
bool readFlag(const std::wstring& file) {
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return false;
    char b[64] = {};
    fgets(b, sizeof(b), f);
    fclose(f);
    return strstr(b, "autoload=1") != nullptr;
}
std::string readLine(const std::wstring& file) {
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return {};
    char b[128] = {};
    fgets(b, sizeof(b), f);
    fclose(f);
    std::string s = b;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}
void writeLine(const std::wstring& file, const std::string& line) {
    FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f) return;
    fwrite(line.data(), 1, line.size(), f);
    fclose(f);
}
std::atomic<bool> g_autoload{false};

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
    std::lock_guard<std::mutex> lock(g_m);
    if (g_edits.empty() || g_level != levelName()) return;
    if (const Edit* ed = editFor(t->name, at)) queueMove(reinterpret_cast<uintptr_t>(entity), t, ed->to);
}

void applyGroupEdits(const std::vector<Edit>& edits);

// Objects that already exist when a layout is loaded: matched by where they are now.
void applyToExisting() {
    std::vector<Edit> edits, previous;
    {
        std::lock_guard<std::mutex> lock(g_m);
        edits = g_edits;
        previous = g_previous;
    }
    if (edits.empty()) return;
    int n = 0;
    for (const auto& [p, t] : inspector::tracked()) {
        if (!t) continue;
        gn_transform now{};
        if (!entityTransform(reinterpret_cast<void*>(p), now)) continue;
        const std::string k = kindOf(t->name);
        // An object a just-unloaded layout moved may not be back yet: it counts as being at its level position.
        float at[3] = {now.trans[0], now.trans[1], now.trans[2]};
        for (const Edit& old : previous)
            if (old.kind == k && dist2(old.to.trans, now.trans) <= kMatch * kMatch) memcpy(at, old.at, sizeof(at));
        for (const Edit& ed : edits)
            if (ed.kind == k && dist2(ed.at, at) <= kMatch * kMatch) {
                queueMove(p, t, ed.to);
                ++n;
                break;
            }
    }
    GN_LOG("layouts: %d existing objects moved by the layout", n);
    server().post([edits] { applyGroupEdits(edits); });
}

// ---- file ---------------------------------------------------------------------------------------------
std::string fmtVec(const float* v) {
    char b[96];
    snprintf(b, sizeof(b), "[%.4f, %.4f, %.4f]", v[0], v[1], v[2]);
    return b;
}

bool save(const std::string& name) {
    std::string level;
    std::vector<Edit> edits;
    {
        std::lock_guard<std::mutex> lock(g_m);
        level = g_level;
        edits = g_edits;
    }
    if (level.empty() || !validName(name)) return false;
    CreateDirectoryW(g_dir.c_str(), nullptr);
    CreateDirectoryW(levelDir(level).c_str(), nullptr);
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
    const std::wstring file = levelDir(level) + widen(name) + L".yaml";
    FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f) {
        GN_LOG("layouts: cannot write the layout for %s", level.c_str());
        return false;
    }
    fwrite(o.data(), 1, o.size(), f);
    fclose(f);
    if (name == "default") DeleteFileW(legacyFile(level).c_str()); // now saved in the level's folder
    writeLine(levelDir(level) + L"last.txt", name);
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_dirty = false;
        g_name = name;
    }
    GN_LOG("layouts: saved %llu edits for %s as \"%s\"", static_cast<unsigned long long>(edits.size()), level.c_str(), name.c_str());
    return true;
}

bool parseVec(const std::string& s, float* v) {
    return sscanf(s.c_str(), " [ %f , %f , %f ]", &v[0], &v[1], &v[2]) == 3;
}

// The level's layout file, if there is one (strict: an edit with a bad line is skipped and logged).
std::vector<Edit> load(const std::string& level, const std::string& name) {
    std::vector<Edit> out;
    FILE* f = _wfopen(fileFor(level, name).c_str(), L"rb");
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
    for (Edit& ed : out)
        if (!ed.haveFrom) { // older files: the level position with the edit's rotation
            ed.from = ed.to;
            memcpy(ed.from.trans, ed.at, sizeof(ed.at));
        }
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
    if (!mem::inImage(setT) || !renderWorld) return;
    alignas(16) gn_transform t = world;
    t.right[3] = t.up[3] = t.forward[3] = 0.f, t.trans[3] = 1.f;
    reinterpret_cast<void (*)(uintptr_t, gn_transform*)>(setT)(obj, &t);
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
    writeInstance(si, local);
    if (ci && ci != si) writeInstance(ci, local);
    if (const uintptr_t mt = memberTransform(g.member, g.copy)) {
        alignas(16) gn_transform row = local;
        row.right[3] = row.up[3] = row.forward[3] = 0.f, row.trans[3] = 1.f;
        mem::write(mt, &row, sizeof(row));
    }
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

// ---- standalone copies of group props (hide and replace) -------------------------------------------
// Entities do not keep their bus; it is in their creation info (+0x10), so GridNode records it when
// a group is created.
void setStatus(const std::string& s);
struct Spawned {
    GroupProp g;
    gn_transform orig{};          // the group copy's own placement
    uintptr_t server = 0, client = 0;
    const reflect::Type* serverType = nullptr;
    const reflect::Type* clientType = nullptr;
};
std::mutex g_spawnM;
std::vector<Spawned> g_spawned; // standalone copies made this level (g_spawnM)

std::mutex g_busM;
std::unordered_map<uintptr_t, uintptr_t> g_groupBus; // group entity -> its bus (g_busM)

void onCreatedGroup(void* entity, void* ci, void*) {
    if (!entity || !ci) return;
    const uintptr_t bus = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(ci) + 0x10);
    if (!bus) return;
    std::lock_guard<std::mutex> lock(g_busM);
    g_groupBus[reinterpret_cast<uintptr_t>(entity)] = bus;
}

uintptr_t busOf(uintptr_t group) {
    std::lock_guard<std::mutex> lock(g_busM);
    const auto it = g_groupBus.find(group);
    return it == g_groupBus.end() ? 0 : it->second;
}

uintptr_t memberType(uintptr_t member) {
    static const uint32_t off = memberField("MemberType");
    return member && off != 0xffffffffu ? mem::readOr<uintptr_t>(member + off) : 0;
}

// Creates and initialises an entity from `data` on the server (as levelSublevel_spawnEntityGroup
// does). Server thread.
uintptr_t spawnServer(uintptr_t data, uintptr_t bus, const gn_transform& at) {
    alignas(16) uint8_t ci[0x200] = {};
    alignas(16) gn_transform t = at;
    t.right[3] = t.up[3] = t.forward[3] = 0.f, t.trans[3] = 1.f;
    reinterpret_cast<void (*)(uint8_t*, uintptr_t, uintptr_t, const gn_transform*)>(mem::rva(e::function::ServerEntityCreationInfo_ctor))(ci, data, bus, &t);
    const uintptr_t entity = reinterpret_cast<uintptr_t (*)(uint8_t*, uintptr_t)>(mem::rva(e::function::entityCreator_spawnIntoBus))(ci, 0);
    if (!entity) return 0;
    alignas(16) uint8_t init[0x40] = {};
    reinterpret_cast<void (*)(uint8_t*, uint64_t, uint64_t)>(mem::rva(e::function::ServerEntityInitInfo_ctor))(init, 0, 0);
    reinterpret_cast<void (*)(uintptr_t, uint8_t*)>(mem::rva(e::function::Entity_init))(entity, init);
    return entity;
}

// The same on the client (as the client group does for a destroyed part). Client thread.
uintptr_t spawnClient(uintptr_t data, uintptr_t bus, const gn_transform& at) {
    alignas(16) uint8_t ci[0x200] = {};
    alignas(16) gn_transform t = at;
    t.right[3] = t.up[3] = t.forward[3] = 0.f, t.trans[3] = 1.f;
    reinterpret_cast<void (*)(uint8_t*, uintptr_t, uintptr_t, const gn_transform*)>(mem::rva(e::function::ClientEntityCreationInfo_ctor))(ci, data, bus, &t);
    const uintptr_t result[1] = {reinterpret_cast<uintptr_t (*)(uint8_t*, uintptr_t)>(mem::rva(e::function::ClientEntity_spawnIntoBus))(ci, 0)};
    const uintptr_t entity = result[0];
    if (!entity) return 0;
    alignas(16) uint8_t init[0x40] = {};
    reinterpret_cast<void (*)(uint8_t*)>(mem::rva(e::function::ClientEntityInitInfo_ctor))(init);
    reinterpret_cast<void (*)(uintptr_t, uint8_t*)>(mem::rva(e::function::Entity_init))(entity, init);
    return entity;
}

void selectSpawned(uintptr_t serverEnt, uintptr_t clientEnt, const gn_transform& orig, const gn_transform& now);

// Server thread: hide the group copy (whose own placement is `orig`) and put a standalone entity of
// its member type at `to`; `select` makes it the editor's selection.
void makeStandalone(const GroupProp& g, const gn_transform& orig, const gn_transform& to, bool select) {
    const uintptr_t data = memberType(g.member);
    const uintptr_t sbus = busOf(g.serverGroup), cbus = busOf(g.clientGroup);
    const reflect::Type* dt = data ? registry().typeOf(mem::ProcessMemory(), data) : nullptr;
    GN_LOG("layouts: make standalone: part %u, data %llx (%s), server bus %llx, client bus %llx", g.part, static_cast<unsigned long long>(data),
           dt ? dt->name.c_str() : "-", static_cast<unsigned long long>(sbus), static_cast<unsigned long long>(cbus));
    if (!data || !sbus || !cbus) {
        if (select) setStatus("Make standalone: the group's data or bus is unknown (reload the level)");
        return;
    }
    const uintptr_t se = spawnServer(data, sbus, to);
    const reflect::Type* st = se ? registry().typeOf(mem::ProcessMemory(), se) : nullptr;
    GN_LOG("layouts: make standalone: server entity %llx (%s)", static_cast<unsigned long long>(se), st ? st->name.c_str() : "-");
    if (!se) {
        if (select) setStatus("Make standalone: the server did not create the entity");
        return;
    }
    // Hide the group copy: its collision and render object far below the level.
    gn_transform away = orig;
    away.trans[1] -= 15000.f;
    moveGroupProp(g, away);
    alignas(16) gn_transform o = orig, w = to;
    client().post([g, data, cbus, o, w, se, select] {
        const uintptr_t ce = spawnClient(data, cbus, w);
        const reflect::Type* ct = ce ? registry().typeOf(mem::ProcessMemory(), ce) : nullptr;
        GN_LOG("layouts: make standalone: client entity %llx (%s)", static_cast<unsigned long long>(ce), ct ? ct->name.c_str() : "-");
        {
            std::lock_guard<std::mutex> lock(g_spawnM);
            Spawned sp;
            sp.g = g;
            sp.orig = o;
            sp.server = se;
            sp.client = ce;
            sp.serverType = registry().typeOf(mem::ProcessMemory(), se);
            sp.clientType = ce ? registry().typeOf(mem::ProcessMemory(), ce) : nullptr;
            g_spawned.push_back(sp);
        }
        if (select) selectSpawned(se, ce, o, w);
    });
}

// The group prop whose own position is `at` (server thread): every copy of every member of every
// group, through the client group's member blocks and the server group body's instances.
bool findGroupProp(const float* at, GroupProp& out, gn_transform& orig) {
    namespace c = e::constant;
    static const reflect::Type* sg = registry().find("ServerStaticModelGroupEntity");
    static const reflect::Type* cg = registry().find("ClientStaticModelGroupEntity");
    static const uint32_t partRange = memberField("PhysicsPartRange"), perCopy = memberField("PhysicsPartCountPerInstance"),
                          copies = memberField("InstanceCount");
    if (!sg || !cg || partRange == 0xffffffffu) return false;
    const auto tracked = inspector::tracked();
    for (const auto& [server, st] : tracked) {
        if (!st || !registry().isKindOf(st, sg)) continue;
        const uintptr_t data = dataOf(server, "StaticModelGroupEntityData");
        uintptr_t client = 0;
        for (const auto& [p, t] : tracked)
            if (t && registry().isKindOf(t, cg) && data && dataOf(p, "StaticModelGroupEntityData") == data) {
                client = p;
                break;
            }
        const uintptr_t serverBody = groupBodyOf(server);
        gn_transform G{};
        if (!client || !serverBody || !entityTransform(reinterpret_cast<void*>(server), G)) continue;
        gn_transform I{};
        I.right[0] = I.up[1] = I.forward[2] = 1.f;
        const uintptr_t blocks = mem::readOr<uintptr_t>(client + c::ClientStaticModelGroup_Members);
        const int32_t count = mem::readOr<int32_t>(client + c::ClientStaticModelGroup_MemberCount);
        for (int32_t b = 0; blocks && b < count && b < 4096; ++b) {
            const uintptr_t block = blocks + static_cast<uintptr_t>(b) * c::GroupMemberBlock_Size;
            const uintptr_t m = mem::readOr<uintptr_t>(block + c::GroupMemberBlock_Member);
            const uint32_t first = m ? mem::readOr<uint32_t>(m + partRange) : 0, per = m ? mem::readOr<uint32_t>(m + perCopy) : 0;
            const uint32_t n = m ? mem::readOr<uint32_t>(m + copies) : 0;
            for (uint32_t k = 0; per && k < n && k < 100000; ++k) {
                gn_transform local{}, world{};
                if (!readInstance(groupInstance(serverBody, first + per * k), local)) continue;
                reframe(I, G, local.trans, true, world.trans);
                if (dist2(world.trans, at) > kMatch * kMatch) continue;
                reframe(I, G, local.right, false, world.right);
                reframe(I, G, local.up, false, world.up);
                reframe(I, G, local.forward, false, world.forward);
                out = GroupProp{};
                out.serverGroup = server;
                out.clientGroup = client;
                out.serverBody = serverBody;
                out.clientBody = groupBodyOf(client);
                out.part = first + per * k;
                out.member = m;
                out.copy = k;
                out.block = block;
                orig = world;
                return true;
            }
        }
    }
    return false;
}

// Server thread: the level's saved standalone copies of group props.
void applyGroupEdits(const std::vector<Edit>& edits) {
    int made = 0, missing = 0;
    for (const Edit& ed : edits) {
        if (ed.kind != "GroupProp") continue;
        Spawned existing{};
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(g_spawnM);
            for (const Spawned& sp : g_spawned)
                if (dist2(sp.orig.trans, ed.at) <= kMatch * kMatch) existing = sp, have = true;
        }
        if (have) { // made standalone earlier this level: move that copy
            if (existing.serverType) queueMove(existing.server, existing.serverType, ed.to);
            if (existing.client && existing.clientType) queueMove(existing.client, existing.clientType, ed.to);
            ++made;
            continue;
        }
        GroupProp g;
        gn_transform orig{};
        if (!findGroupProp(ed.at, g, orig)) {
            ++missing;
            continue;
        }
        makeStandalone(g, orig, ed.to, false);
        ++made;
    }
    if (made || missing) GN_LOG("layouts: %d group props made standalone from the layout, %d not found", made, missing);
}

// ---- the editor (render thread; game work is posted to the realms) -------------------------------------
struct Selected {
    std::string kind;
    float at[3] = {};      // where the level created it (the edit's key)
    gn_transform now{};    // where it is
    gn_transform orig{};   // where the level had it (rotation included)
    std::vector<std::pair<uintptr_t, const reflect::Type*>> entities; // its server and client entity
    bool group = false;    // a prop inside a static model group (experimental; not saved)
    bool spawned = false;  // a standalone copy GridNode made (not saved yet)
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

// ---- loading and unloading layouts ----------------------------------------------------------------------
void clearSelection() {
    std::lock_guard<std::mutex> lock(g_selM);
    g_haveSel = false;
}

// Puts the level back as the game has it: moved objects return to their own placement. A group
// prop's drawn copy cannot be shown again once moved (it is drawn from batches built at load), so a
// standalone copy stays and returns to the group prop's own placement, standing in for it.
void unloadLayout() {
    std::vector<Edit> edits;
    {
        std::lock_guard<std::mutex> lock(g_m);
        edits = g_edits;
        g_previous = edits;
        g_edits.clear();
        g_name.clear();
        g_dirty = false;
    }
    std::vector<Spawned> spawned;
    {
        std::lock_guard<std::mutex> lock(g_spawnM);
        spawned = g_spawned; // they stay: the group copies they replace cannot be shown again
    }
    clearSelection();
    int n = 0;
    for (const auto& [p, t] : inspector::tracked()) {
        if (!t) continue;
        gn_transform now{};
        if (!entityTransform(reinterpret_cast<void*>(p), now)) continue;
        const std::string k = kindOf(t->name);
        for (const Edit& ed : edits)
            if (ed.kind == k && dist2(ed.to.trans, now.trans) <= kMatch * kMatch) {
                queueMove(p, t, ed.from);
                ++n;
                break;
            }
    }
    // A standalone copy stands in for its group prop: back to the prop's own placement.
    for (const Spawned& sp : spawned) {
        if (sp.serverType) queueMove(sp.server, sp.serverType, sp.orig);
        if (sp.client && sp.clientType) queueMove(sp.client, sp.clientType, sp.orig);
    }
    GN_LOG("layouts: unloaded (%d objects put back, %llu standalone copies back in their group prop's place)", n,
           static_cast<unsigned long long>(spawned.size()));
}

// Loads a saved layout of the current level and applies it (unloading what is applied first).
void loadLayout(const std::string& name) {
    std::string level;
    {
        std::lock_guard<std::mutex> lock(g_m);
        level = g_level;
    }
    if (level.empty()) return;
    unloadLayout();
    std::vector<Edit> edits = load(level, name);
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_edits = edits;
        g_name = name;
        g_dirty = false;
    }
    writeLine(levelDir(level) + L"last.txt", name);
    GN_LOG("layouts: loaded \"%s\" (%llu edits) for %s", name.c_str(), static_cast<unsigned long long>(edits.size()), level.c_str());
    std::thread([] { applyToExisting(); }).detach();
}

void deleteLayout(const std::string& name) {
    std::string level;
    {
        std::lock_guard<std::mutex> lock(g_m);
        level = g_level;
        if (g_name == name) g_name.clear(); // what is applied stays applied, unsaved
    }
    DeleteFileW((levelDir(level) + widen(name) + L".yaml").c_str());
    if (name == "default") DeleteFileW(legacyFile(level).c_str());
    if (readLine(levelDir(level) + L"last.txt") == name) DeleteFileW((levelDir(level) + L"last.txt").c_str());
    GN_LOG("layouts: deleted \"%s\" for %s", name.c_str(), level.c_str());
}

// Client thread, after a standalone copy was made: select it (both halves) for editing.
void selectSpawned(uintptr_t serverEnt, uintptr_t clientEnt, const gn_transform& orig, const gn_transform& now) {
    Selected s;
    s.spawned = true;
    for (uintptr_t e : {serverEnt, clientEnt}) {
        const reflect::Type* t = e ? registry().typeOf(mem::ProcessMemory(), e) : nullptr;
        if (t) s.entities.push_back({e, t});
        if (t && s.kind.empty()) s.kind = kindOf(t->name);
    }
    s.orig = orig;
    s.now = now;
    memcpy(s.at, orig.trans, sizeof(s.at));
    char b[200];
    snprintf(b, sizeof(b), "Made a standalone %s (%llu entities) in place of the group prop. Move it as usual.",
             s.kind.c_str(), static_cast<unsigned long long>(s.entities.size()));
    std::lock_guard<std::mutex> lock(g_selM);
    g_sel = s;
    g_haveSel = !s.entities.empty();
    g_status = b;
}

// Server thread: a ray from the camera along its view; the object whose collision it hits.
// `ray` (from, direction; 6 floats) or, if null, the camera's view.
void pick(const float* ray = nullptr) {
    std::array<float, 6> given{};
    const bool haveRay = ray != nullptr;
    if (ray) memcpy(given.data(), ray, sizeof(float) * 6);
    {
        // A new pick replaces the selection, even when it finds nothing to select: otherwise the
        // editor keeps showing (and moving) the previous object under a "not supported" message.
        std::lock_guard<std::mutex> lock(g_selM);
        g_haveSel = false;
        g_status = "Picking...";
    }
    server().post([haveRay, given] {
        gn_transform cam{};
        gn_movement mv{};
        const bool haveMove = movementGet(&mv) == GN_OK;
        float from[3], dir[3];
        if (haveRay) {
            memcpy(from, given.data(), sizeof(from));
            memcpy(dir, given.data() + 3, sizeof(dir));
        } else if (cameraTransform(cam)) {
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
                if ((ed.kind == s.kind || ed.kind == "GroupProp") && dist2(ed.to.trans, s.now.trans) <= kMatch * kMatch) {
                    s.spawned = ed.kind == "GroupProp";
                    memcpy(s.at, ed.at, sizeof(s.at));
                    if (ed.haveFrom) {
                        s.orig = ed.from;
                    } else { // older file: the level's position, the current rotation
                        s.orig = s.now;
                        memcpy(s.orig.trans, ed.at, sizeof(ed.at));
                    }
                }
        }
        {
            std::lock_guard<std::mutex> lock(g_spawnM);
            for (const Spawned& sp : g_spawned)
                if (sp.server == entity) {
                    s.spawned = true;
                    s.orig = sp.orig;
                    memcpy(s.at, sp.orig.trans, sizeof(s.at));
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
// The kind an edit is filed under: standalone copies of group props are "GroupProp", keyed by the
// copy's original position in its group.
std::string editKind(const Selected& s) { return s.spawned ? "GroupProp" : s.kind; }

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
    const std::string key = editKind(s);
    std::lock_guard<std::mutex> lock(g_m);
    for (Edit& ed : g_edits)
        if (ed.kind == key && dist2(ed.at, s.at) <= kMatch * kMatch) {
            ed.to = to;
            g_dirty = true;
            return;
        }
    Edit ed;
    ed.kind = key;
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
    const std::string key = editKind(s);
    std::lock_guard<std::mutex> lock(g_m);
    for (size_t i = 0; i < g_edits.size(); ++i)
        if (g_edits[i].kind == key && dist2(g_edits[i].at, s.at) <= kMatch * kMatch) {
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
    for (const char* cls : {"ServerStaticModelGroupEntity", "ClientStaticModelGroupEntity"})
        onCreated("gridnode", cls, &onCreatedGroup, nullptr);
    g_autoload = readFlag(g_dir + L"settings.txt");
}

// GridNode's thread, when a level starts: with auto-load on, the layout last used for it.
void onLevel(const std::string& level) {
    std::string name;
    if (g_autoload.load()) {
        name = readLine(levelDir(level) + L"last.txt");
        const std::vector<std::string> saved = listLayouts(level);
        if (std::find(saved.begin(), saved.end(), name) == saved.end())
            name = std::find(saved.begin(), saved.end(), "default") != saved.end() ? "default" : "";
    }
    std::vector<Edit> edits = name.empty() ? std::vector<Edit>{} : load(level, name);
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_level = level;
        g_edits = edits;
        g_previous.clear();
        g_name = name;
        g_dirty = false;
    }
    {
        std::lock_guard<std::mutex> lock(g_spawnM);
        g_spawned.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_selM);
        g_haveSel = false;
    }
    g_sweptThisLevel = false;
    {
        std::lock_guard<std::mutex> lock(g_busM);
        g_groupBus.clear();
    }
    if (!name.empty()) GN_LOG("layouts: auto-loading \"%s\" (%llu edits) for %s", name.c_str(), static_cast<unsigned long long>(edits.size()), level.c_str());
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

// ---- free camera (render thread drives it; a camera post-update listener applies it) ---------------
// While the menu is open the game gets no input, so Faith stands still. The free camera writes its
// transform into the view after the game's camera update each frame: GridNode links a node into
// the view manager's post-update listener list (a {vtable, next} list the game walks), no code is
// patched. Right mouse held: look; WASD: move; Q/E: down/up; Shift: faster; Ctrl: slower.
struct CamNode {
    void* const* vtbl;
    CamNode* next;
};
void onCameraUpdated(CamNode* node, void* info);
void camNoop(CamNode*, void*, void*) {} // other slots, should the game call one (e.g. a destructor)
void* const g_camVtbl[8] = {reinterpret_cast<void*>(&onCameraUpdated), reinterpret_cast<void*>(&camNoop), reinterpret_cast<void*>(&camNoop),
                            reinterpret_cast<void*>(&camNoop),         reinterpret_cast<void*>(&camNoop), reinterpret_cast<void*>(&camNoop),
                            reinterpret_cast<void*>(&camNoop),         reinterpret_cast<void*>(&camNoop)};
CamNode g_camNode{g_camVtbl, nullptr};
uintptr_t g_camLinkedTo = 0;         // the view manager the node is in (client thread)

std::mutex g_camM;
bool g_camApply = false;             // (g_camM)
gn_transform g_camT{};               // (g_camM) what the listener writes

struct FreeCam {
    bool enabled = false;
    bool active = false;
    float pos[3] = {};
    float yaw = 0.f, pitch = 0.f;
    float speed = 8.f;               // metres per second
    float fwdSign = -1.f;            // the view's forward row is fwdSign * look direction
    float handed = 1.f;              // right = handed * normalize(cross(Y, look))
    bool looking = false;
};
FreeCam g_cam;                       // render thread
int g_editorFrame = -10;             // last frame the Layout editor was drawn (render thread)

uintptr_t firstViewManager() {
    namespace cv = e::st::ClientCameraViewManagers::off;
    const uintptr_t managers = mem::readOr<uintptr_t>(mem::rva(e::global::ClientCameraViewManagers));
    const uintptr_t begin = managers ? mem::readOr<uintptr_t>(managers + cv::managersBegin) : 0;
    const uintptr_t end = managers ? mem::readOr<uintptr_t>(managers + cv::managersEnd) : 0;
    return begin && end > begin ? mem::readOr<uintptr_t>(begin) : 0;
}

void onCameraUpdated(CamNode*, void*) {
    alignas(16) gn_transform t{};
    {
        std::lock_guard<std::mutex> lock(g_camM);
        if (!g_camApply) return;
        t = g_camT;
    }
    namespace m = e::st::ClientCameraViewManager::off;
    const uintptr_t mgr = firstViewManager();
    if (!mgr) return;
    mem::write(mgr + m::transform, &t, sizeof(t));
    const uintptr_t camera = mem::readOr<uintptr_t>(mgr + m::activeCamera);
    if (camera) mem::write(camera + e::st::CameraObject::off::transform, &t, sizeof(t));
}

// Client thread: link / unlink the node at the end of the listener list.
void linkCameraNode() {
    namespace m = e::st::ClientCameraViewManager::off;
    const uintptr_t mgr = firstViewManager();
    if (!mgr) return;
    const uintptr_t headAt = mgr + m::postUpdateListeners;
    for (uintptr_t q = mem::readOr<uintptr_t>(headAt), guard = 0; q && guard < 1000; q = mem::readOr<uintptr_t>(q + 8), ++guard)
        if (q == reinterpret_cast<uintptr_t>(&g_camNode)) return; // already in this manager's list
    g_camNode.next = nullptr;
    uintptr_t p = mem::readOr<uintptr_t>(headAt);
    if (!p) {
        mem::write<uintptr_t>(headAt, reinterpret_cast<uintptr_t>(&g_camNode));
    } else {
        for (int guard = 0; guard < 1000; ++guard) {
            const uintptr_t next = mem::readOr<uintptr_t>(p + 8);
            if (!next) break;
            p = next;
        }
        mem::write<uintptr_t>(p + 8, reinterpret_cast<uintptr_t>(&g_camNode));
    }
    g_camLinkedTo = mgr;
}

void unlinkCameraNode() {
    namespace m = e::st::ClientCameraViewManager::off;
    const uintptr_t mgr = g_camLinkedTo;
    g_camLinkedTo = 0;
    if (!mgr || mgr != firstViewManager()) return; // that manager is gone with its list
    const uintptr_t self = reinterpret_cast<uintptr_t>(&g_camNode);
    uintptr_t at = mgr + m::postUpdateListeners; // the address holding the pointer to the current node
    for (int guard = 0; guard < 1000; ++guard) {
        const uintptr_t p = mem::readOr<uintptr_t>(at);
        if (!p) return;
        if (p == self) {
            mem::write<uintptr_t>(at, reinterpret_cast<uintptr_t>(g_camNode.next));
            return;
        }
        at = p + 8;
    }
}

void lookDir(float yaw, float pitch, float* d) {
    d[0] = std::cos(pitch) * std::sin(yaw);
    d[1] = std::sin(pitch);
    d[2] = std::cos(pitch) * std::cos(yaw);
}

void cross(const float* a, const float* b, float* o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The free camera's basis: look direction, screen right and screen up.
void camBasis(const FreeCam& c, float* d, float* r, float* u) {
    lookDir(c.yaw, c.pitch, d);
    const float Y[3] = {0.f, 1.f, 0.f};
    cross(Y, d, r);
    normalize(r);
    for (int k = 0; k < 3; ++k) r[k] *= c.handed;
    cross(d, r, u);
    normalize(u);
    if (u[1] < 0.f)
        for (int k = 0; k < 3; ++k) u[k] = -u[k];
}

// Starts from the game's current view.
bool startFreeCam() {
    gn_transform v{};
    if (!cameraTransform(v)) return false;
    // MECMP: the view looks along minus forward; check against the character camera when it is there.
    float d[3] = {-v.forward[0], -v.forward[1], -v.forward[2]};
    g_cam.fwdSign = -1.f;
    gn_movement mv{};
    if (movementGet(&mv) == GN_OK && dot3(d, mv.camera_forward) < 0.f) {
        for (float& x : d) x = -x;
        g_cam.fwdSign = 1.f;
    }
    normalize(d);
    g_cam.yaw = std::atan2(d[0], d[2]);
    g_cam.pitch = std::asin(std::clamp(d[1], -1.f, 1.f));
    memcpy(g_cam.pos, v.trans, sizeof(g_cam.pos));
    const float Y[3] = {0.f, 1.f, 0.f};
    float r[3];
    cross(Y, d, r);
    g_cam.handed = dot3(r, v.right) >= 0.f ? 1.f : -1.f;
    return true;
}

// Render thread, every frame.
void freeCamFrame(bool menuOpen) {
    const bool want = g_cam.enabled && menuOpen && localClientCharacter() != nullptr;
    if (want && !g_cam.active) {
        if (!startFreeCam()) return;
        g_cam.active = true;
        client().post([] { linkCameraNode(); });
        GN_LOG("layouts: free camera on at (%.1f, %.1f, %.1f)", g_cam.pos[0], g_cam.pos[1], g_cam.pos[2]);
    } else if (!want && g_cam.active) {
        g_cam.active = false;
        g_cam.looking = false;
        ui::setLookCapture(false);
        {
            std::lock_guard<std::mutex> lock(g_camM);
            g_camApply = false;
        }
        client().post([] { unlinkCameraNode(); });
        GN_LOG("layouts: free camera off");
    }
    if (!g_cam.active) return;
    if (ImGui::GetFrameCount() % 60 == 0) client().post([] { linkCameraNode(); }); // a level change makes a new view manager
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !io.WantCaptureMouse) g_cam.looking = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) g_cam.looking = false;
    ui::setLookCapture(g_cam.looking);
    float dx = 0.f, dy = 0.f;
    ui::takeLook(dx, dy);
    float d[3], r[3], u[3];
    camBasis(g_cam, d, r, u);
    // Turning toward screen right: the sign that makes d(look)/d(yaw) point along `right`.
    const float dYaw[3] = {std::cos(g_cam.pitch) * std::cos(g_cam.yaw), 0.f, -std::cos(g_cam.pitch) * std::sin(g_cam.yaw)};
    const float yawSign = dot3(dYaw, r) >= 0.f ? 1.f : -1.f;
    constexpr float kSens = 0.0025f, kPitchMax = 1.5f;
    g_cam.yaw += yawSign * dx * kSens;
    g_cam.pitch = std::clamp(g_cam.pitch - dy * kSens, -kPitchMax, kPitchMax);
    camBasis(g_cam, d, r, u);
    if (!io.WantTextInput) {
        float v[3] = {};
        auto add = [&](const float* a, float s) {
            for (int k = 0; k < 3; ++k) v[k] += a[k] * s;
        };
        const float Y[3] = {0.f, 1.f, 0.f};
        if (ImGui::IsKeyDown(ImGuiKey_W)) add(d, 1.f);
        if (ImGui::IsKeyDown(ImGuiKey_S)) add(d, -1.f);
        if (ImGui::IsKeyDown(ImGuiKey_D)) add(r, 1.f);
        if (ImGui::IsKeyDown(ImGuiKey_A)) add(r, -1.f);
        if (ImGui::IsKeyDown(ImGuiKey_E)) add(Y, 1.f);
        if (ImGui::IsKeyDown(ImGuiKey_Q)) add(Y, -1.f);
        float s = g_cam.speed * std::min(io.DeltaTime, 0.1f);
        if (io.KeyShift) s *= 4.f;
        if (io.KeyCtrl) s *= 0.25f;
        for (int k = 0; k < 3; ++k) g_cam.pos[k] += v[k] * s;
        if (io.MouseWheel != 0.f && g_cam.looking) g_cam.speed = std::clamp(g_cam.speed * (io.MouseWheel > 0 ? 1.25f : 0.8f), 0.5f, 200.f);
    }
    alignas(16) gn_transform t{};
    for (int k = 0; k < 3; ++k) {
        t.right[k] = r[k];
        t.up[k] = u[k];
        t.forward[k] = g_cam.fwdSign * d[k];
        t.trans[k] = g_cam.pos[k];
    }
    t.trans[3] = 1.f;
    std::lock_guard<std::mutex> lock(g_camM);
    g_camT = t;
    g_camApply = true;
}

// The ray through the cursor, from the current view (free camera or the game's).
bool cursorRay(float* ray) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x < 1.f || io.DisplaySize.y < 1.f) return false;
    float from[3], d[3], r[3], u[3];
    if (g_cam.active) {
        camBasis(g_cam, d, r, u);
        memcpy(from, g_cam.pos, sizeof(from));
    } else {
        gn_transform v{};
        if (!cameraTransform(v)) return false;
        for (int k = 0; k < 3; ++k) d[k] = -v.forward[k], r[k] = v.right[k], u[k] = v.up[k], from[k] = v.trans[k];
        normalize(d), normalize(r), normalize(u);
    }
    const uintptr_t mgr = firstViewManager();
    const float fovDeg = mgr ? mem::readOr<float>(mgr + e::st::ClientCameraViewManager::off::fovYDegrees) : 0.f;
    const float fov = (fovDeg > 10.f && fovDeg < 170.f ? fovDeg : 70.f) * 3.14159265f / 180.f;
    const float aspect = io.DisplaySize.x / io.DisplaySize.y;
    const float nx = 2.f * io.MousePos.x / io.DisplaySize.x - 1.f, ny = 1.f - 2.f * io.MousePos.y / io.DisplaySize.y;
    const float th = std::tan(fov * 0.5f);
    for (int k = 0; k < 3; ++k) ray[3 + k] = d[k] + r[k] * nx * th * aspect + u[k] * ny * th;
    normalize(ray + 3);
    memcpy(ray, from, sizeof(from));
    return true;
}

void draw(bool* open) {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Layout editor", open)) {
        ImGui::End();
        return;
    }
    size_t count;
    bool dirty;
    std::string level, loaded;
    {
        std::lock_guard<std::mutex> lock(g_m);
        count = g_edits.size();
        dirty = g_dirty;
        level = g_level;
        loaded = g_name;
    }
    ImGui::TextDisabled("%s", level.empty() ? "no level" : level.c_str());
    g_editorFrame = ImGui::GetFrameCount();
    ImGui::SeparatorText("Camera");
    ImGui::Checkbox("Free camera", &g_cam.enabled);
    if (g_cam.enabled) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::SliderFloat("speed", &g_cam.speed, 0.5f, 200.f, "%.1f m/s", ImGuiSliderFlags_Logarithmic);
        ImGui::TextDisabled("Hold right mouse to look; WASD move, Q/E down/up, Shift faster, Ctrl slower; wheel while looking: speed.");
    }
    ImGui::TextDisabled("Left-click an object in the world to pick it.");
    ImGui::SeparatorText("Layouts");
    if (loaded.empty()) ImGui::Text("%s", count ? "Unsaved edits" : "The level as the game has it");
    else ImGui::Text("Layout \"%s\"", loaded.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%llu edit%s%s", static_cast<unsigned long long>(count), count == 1 ? "" : "s", dirty ? ", not saved" : "");
    static char nameBuf[64] = "default";
    static std::string shownFor;
    if (shownFor != level + "|" + loaded) { // follow the loaded layout's name
        shownFor = level + "|" + loaded;
        if (!loaded.empty()) snprintf(nameBuf, sizeof(nameBuf), "%s", loaded.c_str());
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputText("##name", nameBuf, sizeof(nameBuf));
    ImGui::SameLine();
    const bool nameOk = validName(nameBuf);
    ImGui::BeginDisabled(!nameOk || level.empty());
    if (ImGui::Button("Save as")) save(nameBuf);
    ImGui::EndDisabled();
    if (!nameOk) ImGui::TextDisabled("Names: letters, digits, space, - _ . (up to 60)");
    bool autoload = g_autoload.load();
    if (ImGui::Checkbox("Load the last used layout when a level starts", &autoload)) {
        g_autoload = autoload;
        CreateDirectoryW(g_dir.c_str(), nullptr);
        writeLine(g_dir + L"settings.txt", autoload ? "autoload=1\n" : "autoload=0\n");
    }
    // The level's saved layouts (listed again at most once a second).
    static std::vector<std::string> saved;
    static double listedAt = -10.0;
    static std::string listedFor;
    if (ImGui::GetTime() - listedAt > 1.0 || listedFor != level) {
        saved = level.empty() ? std::vector<std::string>{} : listLayouts(level);
        listedAt = ImGui::GetTime();
        listedFor = level;
    }
    static std::string confirmDelete;
    for (const std::string& n : saved) {
        ImGui::PushID(n.c_str());
        ImGui::BulletText("%s%s", n.c_str(), n == loaded ? " (loaded)" : "");
        ImGui::SameLine();
        if (ImGui::SmallButton("Load")) loadLayout(n);
        ImGui::SameLine();
        if (confirmDelete == n) {
            if (ImGui::SmallButton("Really delete")) {
                deleteLayout(n);
                confirmDelete.clear();
                listedAt = -10.0;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Keep")) confirmDelete.clear();
        } else if (ImGui::SmallButton("Delete")) {
            confirmDelete = n;
        }
        ImGui::PopID();
    }
    if (saved.empty()) ImGui::TextDisabled("No saved layouts for this level.");
    ImGui::BeginDisabled(count == 0 && loaded.empty());
    if (ImGui::Button("Unload (put the level back)")) unloadLayout();
    ImGui::EndDisabled();
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
    if (have && s.group) {
        ImGui::TextWrapped("This prop is part of a static model group, which the game draws as one batch. Make it standalone to move it: "
                           "the group's copy is hidden and a standalone copy takes its place.");
        if (ImGui::Button("Make standalone")) {
            const GroupProp gp = s.gp;
            const gn_transform at = s.orig;
            setStatus("Making a standalone copy...");
            server().post([gp, at] { makeStandalone(gp, at, at, true); });
        }
    } else if (have) {
        ImGui::Text("%s%s  (%llu entities)", s.kind.c_str(), s.spawned ? " (standalone copy of a group prop)" : "",
                    static_cast<unsigned long long>(s.entities.size()));
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

void frame(bool menuOpen) {
    freeCamFrame(menuOpen);
    // Click to pick: a left click on the world (not on a window) while the Layout editor is open.
    const ImGuiIO& io = ImGui::GetIO();
    if (menuOpen && ImGui::GetFrameCount() - g_editorFrame <= 2 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse) {
        float ray[6];
        if (cursorRay(ray)) pick(ray);
    }
}

} // namespace gn::host::layouts
