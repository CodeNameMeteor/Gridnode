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
    // kind "Copy": a new object made from another one. `source` is that object's kind ("GroupProp" for
    // a prop in a static model group) and `at` its level position; copyId tells copies apart (not saved).
    std::string source;
    int copyId = -1;
};

std::mutex g_m;
std::string g_level;            // the level the edits are for (g_m)
std::vector<Edit> g_edits;      // (g_m)
bool g_dirty = false;           // unsaved changes (g_m)
std::wstring g_dir;             // layouts folder
std::string g_name;             // the loaded / last saved layout's name, empty if none (g_m)
std::vector<Edit> g_previous;   // the edits of the layout unloaded last, to find what they moved (g_m)
int g_nextCopyId = 0;           // (g_m)

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

// Key binds (layouts\settings.txt, "key_<id>=<name>"). Free-camera binds work only while it flies.
struct Bind {
    const char* id;
    const char* label;
    int def;
    bool freeCamOnly;
    bool repeat; // repeats while held
    int vk;
};
Bind g_binds[] = {
    {"freecam", "Free camera on / off", VK_F7, false, false, VK_F7},
    {"pick", "Pick what the crosshair points at", 'R', true, false, 'R'},
    {"copy", "Copy the selected object", 'C', true, false, 'C'},
    {"paste", "Paste a copy at the crosshair", 'V', true, false, 'V'},
    {"duplicate", "Duplicate the selected object in place", 'G', true, false, 'G'},
    {"forward", "Move away from the camera", VK_UP, true, true, VK_UP},
    {"back", "Move toward the camera", VK_DOWN, true, true, VK_DOWN},
    {"left", "Move left", VK_LEFT, true, true, VK_LEFT},
    {"right", "Move right", VK_RIGHT, true, true, VK_RIGHT},
    {"up", "Move up", VK_PRIOR, true, true, VK_PRIOR},
    {"down", "Move down", VK_NEXT, true, true, VK_NEXT},
    {"turnleft", "Turn left", 'Z', true, true, 'Z'},
    {"turnright", "Turn right", 'X', true, true, 'X'},
    {"tiltleft", "Tilt left", 'N', true, true, 'N'},
    {"tiltright", "Tilt right", 'M', true, true, 'M'},
    {"tipaway", "Tip away", 'K', true, true, 'K'},
    {"tiptoward", "Tip toward", 'L', true, true, 'L'},
    {"flip", "Flip upside down", 'H', true, false, 'H'},
    {"upright", "Stand upright again", 'U', true, false, 'U'},
    {"undo", "Undo (with Ctrl; also with the menu open)", 'Z', true, false, 'Z'},
    {"teleport", "Teleport Faith to the crosshair (Shift: and go back to her)", 'T', true, false, 'T'},
    {"pointer", "Mouse: look / pointer (to drag the handles)", VK_TAB, true, false, VK_TAB},
    {"handles", "Handles: move / rotate", 'Y', true, false, 'Y'},
};
enum BindId { kFreeCam, kPick, kCopy, kPaste, kDuplicate, kFwd, kBack, kLeft, kRight, kUp, kDown, kTurnL, kTurnR, kTiltL, kTiltR, kTipAway, kTipToward, kFlip, kUpright, kUndo, kTeleport, kPointer, kHandles, kBindCount };
static_assert(sizeof(g_binds) / sizeof(g_binds[0]) == kBindCount, "binds and their ids");
float g_nudge = 0.5f;     // metres per move step
float g_turnStep = 15.f;  // degrees per turn step

void saveSettings() {
    CreateDirectoryW(g_dir.c_str(), nullptr);
    std::string o = g_autoload.load() ? "autoload=1\n" : "autoload=0\n";
    char b[64];
    snprintf(b, sizeof(b), "nudge=%.3f\nturn=%.1f\n", g_nudge, g_turnStep);
    o += b;
    for (const Bind& k : g_binds) o += std::string("key_") + k.id + "=" + (k.vk ? ui::keyName(k.vk) : "NONE") + "\n";
    writeLine(g_dir + L"settings.txt", o);
}

void loadSettings() {
    FILE* f = _wfopen((g_dir + L"settings.txt").c_str(), L"rb");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "autoload") g_autoload = v == "1";
        else if (k == "nudge") g_nudge = std::clamp(static_cast<float>(atof(v.c_str())), 0.01f, 100.f);
        else if (k == "turn") g_turnStep = std::clamp(static_cast<float>(atof(v.c_str())), 0.1f, 180.f);
        else if (k.rfind("key_", 0) == 0)
            for (Bind& b : g_binds)
                if (k.substr(4) == b.id) b.vk = v == "NONE" ? 0 : (ui::keyFromName(v) ? ui::keyFromName(v) : b.vk);
    }
    fclose(f);
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
    if (moved != total || said.fetch_add(1) < 2)
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

void recordBus(uintptr_t entity, uintptr_t bus);
std::atomic<uint32_t> g_groupGen{0}; // bumped when a model or group is created (part of the level loaded)

// Creation of a static model (any realm): match against the layout by its creation transform.
void onCreatedModel(void* entity, void* ci, void*) {
    if (!entity || !ci) return;
    recordBus(reinterpret_cast<uintptr_t>(entity), mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(ci) + 0x10)); // for copies
    g_groupGen.fetch_add(1);
    const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), reinterpret_cast<uintptr_t>(entity));
    if (!t) return;
    float at[3];
    if (!mem::read(reinterpret_cast<uintptr_t>(ci) + e::st::EntityCreationInfo::off::transform + 0x30, at, sizeof(at))) return;
    std::lock_guard<std::mutex> lock(g_m);
    if (g_edits.empty() || g_level != levelName()) return;
    if (const Edit* ed = editFor(t->name, at)) queueMove(reinterpret_cast<uintptr_t>(entity), t, ed->to);
}

void applyGroupEdits(const std::vector<Edit>& edits, bool onlyMissing = false);

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
        if (!ed.source.empty()) o += "    source: " + ed.source + "\n";
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
        if (key == "source") {
            cur.source = val;
            continue;
        }
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
    for (Edit& ed : out) {
        if (ed.kind == "Copy") { // a copy has no level placement: putting it back removes it
            ed.from = ed.to;
            ed.haveFrom = true;
            std::lock_guard<std::mutex> lock(g_m);
            ed.copyId = g_nextCopyId++;
        } else if (!ed.haveFrom) { // older files: the level position with the edit's rotation
            ed.from = ed.to;
            memcpy(ed.from.trans, ed.at, sizeof(ed.at));
        }
        if (ed.kind == "Copy" && ed.source.empty()) GN_LOG("layouts: %s: a copy without a source; skipped", level.c_str());
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const Edit& ed) { return ed.kind == "Copy" && ed.source.empty(); }), out.end());
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
    // What it was made from (for copying it again).
    uintptr_t data = 0, serverBus = 0, clientBus = 0;
    int copyId = -1;              // >= 0: a copy (Edit kind "Copy"); -1: stands in for a group prop
    bool hidden = false;          // a removed copy, parked below the level for reuse
    std::string source;           // a copy's source kind and level position
    float sourceAt[3] = {};
    // The entities the data and buses belong to (a group or a static model): when the part of the
    // level they are in streams out, the data and buses go with it.
    uintptr_t ownerServer = 0, ownerClient = 0;
};
std::mutex g_spawnM;
std::vector<Spawned> g_spawned; // standalone copies made this level (g_spawnM)

std::mutex g_busM;
std::unordered_map<uintptr_t, uintptr_t> g_groupBus; // group entity -> its bus (g_busM)

void recordBus(uintptr_t entity, uintptr_t bus) {
    if (!entity || !bus) return;
    std::lock_guard<std::mutex> lock(g_busM);
    g_groupBus[entity] = bus;
}


void onCreatedGroup(void* entity, void* ci, void*) {
    if (!entity || !ci) return;
    g_groupGen.fetch_add(1);
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
            sp.data = data;
            sp.serverBus = busOf(g.serverGroup);
            sp.clientBus = cbus;
            sp.ownerServer = g.serverGroup, sp.ownerClient = g.clientGroup;
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
    // Client groups by their data, once (pairing each server group by scanning everything was slow).
    std::unordered_map<uintptr_t, uintptr_t> clientByData;
    std::vector<uintptr_t> servers;
    for (const auto& [p, t] : tracked) {
        if (!t) continue;
        if (registry().isKindOf(t, cg)) {
            if (const uintptr_t d = dataOf(p, "StaticModelGroupEntityData")) clientByData.emplace(d, p);
        } else if (registry().isKindOf(t, sg))
            servers.push_back(p);
    }
    for (const uintptr_t server : servers) {
        const uintptr_t data = dataOf(server, "StaticModelGroupEntityData");
        const auto ci = data ? clientByData.find(data) : clientByData.end();
        const uintptr_t client = ci == clientByData.end() ? 0 : ci->second;
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

// ---- copies: new objects made from an existing one --------------------------------------------------
// A template is what a new copy is made from: the object's data (its StaticModelEntityData) and the
// entity buses of its server and client halves, plus where it came from, which is how a layout file
// names it (the source's kind and level position).
struct Template {
    uintptr_t data = 0, serverBus = 0, clientBus = 0;
    std::string source;   // "GroupProp" or the object's kind
    float at[3] = {};     // the source's level position
    gn_transform place{}; // the source's placement when copied (rotation for pasting)
    std::string label;
    uintptr_t ownerServer = 0, ownerClient = 0; // whose data and buses these are (see Spawned)
    const reflect::Type* ownerServerType = nullptr;
    const reflect::Type* ownerClientType = nullptr;
};

void setOwners(Template& t, uintptr_t server, uintptr_t client) {
    t.ownerServer = server, t.ownerClient = client;
    t.ownerServerType = server ? registry().typeOf(mem::ProcessMemory(), server) : nullptr;
    t.ownerClientType = client ? registry().typeOf(mem::ProcessMemory(), client) : nullptr;
}

// Still usable: its owners are alive (an entity's vtable still says its class) and its data is
// still a StaticModelEntityData. A streamed-out part of the level frees all three.
bool templateAlive(const Template& t) {
    static const reflect::Type* smd = registry().find("StaticModelEntityData");
    if (!t.ownerServer || !t.ownerClient || !t.ownerServerType || !t.ownerClientType) return false;
    if (registry().typeOf(mem::ProcessMemory(), t.ownerServer) != t.ownerServerType) return false;
    if (registry().typeOf(mem::ProcessMemory(), t.ownerClient) != t.ownerClientType) return false;
    const reflect::Type* dt = t.data ? registry().typeOf(mem::ProcessMemory(), t.data) : nullptr;
    return dt && smd && registry().isKindOf(dt, smd);
}

void selectCopy(const Spawned& sp, const gn_transform& now, bool add = false);
void hideCopy(int copyId);

// The template for a source named in a layout file (server thread).
bool resolveSource(const std::string& source, const float* at, Template& t) {
    t = Template{};
    t.source = source;
    memcpy(t.at, at, sizeof(t.at));
    if (source == "GroupProp") {
        {   // made standalone already: its group copy is hidden, the standalone copy knows the data
            std::lock_guard<std::mutex> lock(g_spawnM);
            for (const Spawned& sp : g_spawned)
                if (sp.copyId < 0 && dist2(sp.orig.trans, at) <= kMatch * kMatch && sp.data) {
                    t.data = sp.data, t.serverBus = sp.serverBus, t.clientBus = sp.clientBus, t.place = sp.orig;
                    setOwners(t, sp.ownerServer, sp.ownerClient);
                    return templateAlive(t);
                }
        }
        GroupProp g;
        gn_transform orig{};
        if (!findGroupProp(at, g, orig)) return false;
        t.data = memberType(g.member);
        t.serverBus = busOf(g.serverGroup);
        t.clientBus = busOf(g.clientGroup);
        t.place = orig;
        setOwners(t, g.serverGroup, g.clientGroup);
        return t.data && t.serverBus && t.clientBus && templateAlive(t);
    }
    // A standalone object of the level: where it is now (moved by an edit, or where the level put it).
    float now[3] = {at[0], at[1], at[2]};
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (const Edit& ed : g_edits)
            if (ed.kind == source && dist2(ed.at, at) <= kMatch * kMatch) memcpy(now, ed.to.trans, sizeof(now));
    }
    uintptr_t server = 0, client = 0;
    for (const auto& [p, ty] : inspector::tracked()) {
        if (!ty || kindOf(ty->name) != source) continue;
        gn_transform tr{};
        if (!entityTransform(reinterpret_cast<void*>(p), tr) || dist2(tr.trans, now) > kMatch * kMatch) continue;
        (isServerClass(ty->name) ? server : client) = p;
        if (isServerClass(ty->name)) t.place = tr;
    }
    t.data = server ? dataOf(server, "StaticModelEntityData") : 0;
    t.serverBus = server ? busOf(server) : 0;
    t.clientBus = client ? busOf(client) : 0;
    setOwners(t, server, client);
    return t.data && t.serverBus && t.clientBus && templateAlive(t);
}

// Server thread: a copy of `t` at `to`. A removed copy of the same object is reused when there is one.
void spawnCopy(const Template& t, const gn_transform& to, int copyId, bool select, bool addSel = false) {
    if (!templateAlive(t)) {
        if (select) GN_LOG("layouts: copy %d of %s: the part of the level it comes from is no longer loaded", copyId, t.source.c_str());
        if (select) setStatus("That object's part of the level is no longer loaded (it streams in and out as you move). "
                              "Fly back to it and copy it again.");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_spawnM);
        for (Spawned& sp : g_spawned)
            if (sp.hidden && sp.data == t.data && sp.serverType && registry().typeOf(mem::ProcessMemory(), sp.server) == sp.serverType) {
                sp.hidden = false;
                sp.copyId = copyId;
                sp.source = t.source;
                memcpy(sp.sourceAt, t.at, sizeof(sp.sourceAt));
                if (sp.serverType) queueMove(sp.server, sp.serverType, to);
                if (sp.client && sp.clientType) queueMove(sp.client, sp.clientType, to);
                if (select) {
                    const Spawned copy = sp;
                    client().post([copy, to, addSel] { selectCopy(copy, to, addSel); });
                }
                return;
            }
    }
    const uintptr_t se = spawnServer(t.data, t.serverBus, to);
    GN_LOG("layouts: copy %d of %s at (%.2f, %.2f, %.2f): server entity %llx", copyId, t.source.c_str(), to.trans[0], to.trans[1], to.trans[2],
           static_cast<unsigned long long>(se));
    if (!se) {
        if (select) setStatus("Paste: the server did not create the entity");
        return;
    }
    Spawned sp;
    sp.server = se;
    sp.serverType = registry().typeOf(mem::ProcessMemory(), se);
    sp.orig = to;
    sp.data = t.data, sp.serverBus = t.serverBus, sp.clientBus = t.clientBus;
    sp.copyId = copyId;
    sp.source = t.source;
    memcpy(sp.sourceAt, t.at, sizeof(sp.sourceAt));
    sp.ownerServer = t.ownerServer, sp.ownerClient = t.ownerClient;
    {   // recorded now (its client half follows), so nothing places it a second time meanwhile
        std::lock_guard<std::mutex> lock(g_spawnM);
        g_spawned.push_back(sp);
    }
    alignas(16) gn_transform w = to;
    client().post([sp, w, select, addSel]() mutable {
        sp.client = spawnClient(sp.data, sp.clientBus, w);
        sp.clientType = sp.client ? registry().typeOf(mem::ProcessMemory(), sp.client) : nullptr;
        {
            std::lock_guard<std::mutex> lock(g_spawnM);
            for (Spawned& rec : g_spawned)
                if (rec.server == sp.server) rec.client = sp.client, rec.clientType = sp.clientType;
        }
        if (select) selectCopy(sp, w, addSel);
    });
}

// Server thread: the level's saved copies and standalone copies of group props. With onlyMissing,
// only what is not there (any more) is placed, quietly when nothing can be: after part of the level
// streamed out and back in, which destroys the copies made in it.
void applyGroupEdits(const std::vector<Edit>& edits, bool onlyMissing) {
    int made = 0, missing = 0, copies = 0, lost = 0;
    std::unordered_map<std::string, std::pair<bool, Template>> resolved; // one lookup per source
    // Copies first: a copy of a group prop finds its source in the group before that prop is made standalone.
    for (const Edit& ed : edits) {
        if (ed.kind != "Copy") continue;
        bool moved = false;
        {
            std::lock_guard<std::mutex> lock(g_spawnM);
            for (const Spawned& sp : g_spawned)
                if (sp.copyId == ed.copyId && !sp.hidden) {
                    if (!onlyMissing) {
                        if (sp.serverType) queueMove(sp.server, sp.serverType, ed.to);
                        if (sp.client && sp.clientType) queueMove(sp.client, sp.clientType, ed.to);
                    }
                    moved = true;
                }
        }
        if (moved) {
            if (!onlyMissing) ++copies;
            continue;
        }
        Template t;
        char key[160];
        snprintf(key, sizeof(key), "%s|%.3f|%.3f|%.3f", ed.source.c_str(), ed.at[0], ed.at[1], ed.at[2]);
        auto memo = resolved.find(key);
        if (memo == resolved.end()) memo = resolved.emplace(key, std::make_pair(resolveSource(ed.source, ed.at, t), t)).first;
        t = memo->second.second;
        if (!memo->second.first) {
            if (!onlyMissing) GN_LOG("layouts: copy of %s at (%.2f, %.2f, %.2f): the source was not found", ed.source.c_str(), ed.at[0], ed.at[1], ed.at[2]);
            ++lost;
            continue;
        }
        spawnCopy(t, ed.to, ed.copyId, false);
        ++copies;
    }
    if (onlyMissing ? copies > 0 : (copies || lost))
        GN_LOG("layouts: %d copies placed%s, %d sources not found", copies, onlyMissing ? " again (their part of the level was reloaded)" : " from the layout", lost);
    for (const Edit& ed : edits) {
        if (ed.kind != "GroupProp") continue;
        Spawned existing{};
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(g_spawnM);
            for (const Spawned& sp : g_spawned)
                if (sp.copyId < 0 && !sp.hidden && dist2(sp.orig.trans, ed.at) <= kMatch * kMatch) existing = sp, have = true;
        }
        if (have) { // made standalone earlier this level: move that copy
            if (onlyMissing) continue;
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
    if (onlyMissing ? made > 0 : (made || missing))
        GN_LOG("layouts: %d group props made standalone%s, %d not found", made, onlyMissing ? " again (their part of the level was reloaded)" : " from the layout",
               missing);
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
    int copyId = -1;       // >= 0: a copy (Edit kind "Copy")
    GroupProp gp;
};
std::mutex g_selM;
Selected g_sel;            // (g_selM)
bool g_haveSel = false;    // (g_selM)
std::vector<Selected> g_extra; // (g_selM) the rest of a multiple selection (Shift+pick); g_sel leads
std::string g_status;      // (g_selM)

void setStatus(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_selM);
    g_status = s;
}

// Whether two selections are the same object.
bool sameObject(const Selected& a, const Selected& b) {
    if (a.group || b.group) return a.group && b.group && a.gp.serverGroup == b.gp.serverGroup && a.gp.part == b.gp.part;
    if (a.copyId >= 0 || b.copyId >= 0) return a.copyId == b.copyId;
    for (const auto& x : a.entities)
        for (const auto& y : b.entities)
            if (x.first == y.first) return true;
    return false;
}

// g_selM held: `s` becomes the selection, or with `add` joins it (and leaves it when it is already in).
void setPickedLocked(const Selected& s, bool add) {
    if (!add || !g_haveSel) {
        g_extra.clear();
        g_sel = s;
        g_haveSel = true;
        return;
    }
    if (sameObject(g_sel, s)) {
        if (g_extra.empty()) {
            g_haveSel = false;
        } else {
            g_sel = g_extra.back();
            g_extra.pop_back();
        }
        return;
    }
    for (size_t i = 0; i < g_extra.size(); ++i)
        if (sameObject(g_extra[i], s)) {
            g_extra.erase(g_extra.begin() + static_cast<long>(i));
            return;
        }
    g_extra.push_back(g_sel);
    g_sel = s;
}

// Everything selected, the leading object first.
std::vector<Selected> selectionAll() {
    std::lock_guard<std::mutex> lock(g_selM);
    std::vector<Selected> out;
    if (!g_haveSel) return out;
    out.push_back(g_sel);
    out.insert(out.end(), g_extra.begin(), g_extra.end());
    return out;
}

// The group props in the selection made standalone (so they can move); false if there were none.
bool standaloneGroupProps() {
    int n = 0;
    for (const Selected& s : selectionAll())
        if (s.group) {
            const GroupProp gp = s.gp;
            const gn_transform at = s.orig;
            server().post([gp, at] { makeStandalone(gp, at, at, true); });
            ++n;
        }
    if (n) setStatus(n == 1 ? "Making the group prop standalone so it can move..." : "Making the group props standalone so they can move...");
    return n > 0;
}

// g_selM held: " (N selected)" when more than one is.
std::string countNoteLocked() {
    if (!g_haveSel || g_extra.empty()) return "";
    return " (" + std::to_string(g_extra.size() + 1) + " selected)";
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

void clearUndo();

// ---- loading and unloading layouts ----------------------------------------------------------------------
void clearSelection() {
    std::lock_guard<std::mutex> lock(g_selM);
    g_haveSel = false;
    g_extra.clear();
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
    clearUndo(); // the steps were for the layout that is gone
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
    // Copies go away; a standalone copy stands in for its group prop: back to the prop's own placement.
    for (const Spawned& sp : spawned)
        if (sp.copyId >= 0) hideCopy(sp.copyId);
    for (const Spawned& sp : spawned) {
        if (sp.copyId >= 0 || sp.hidden) continue;
        if (sp.serverType) queueMove(sp.server, sp.serverType, sp.orig);
        if (sp.client && sp.clientType) queueMove(sp.client, sp.clientType, sp.orig);
    }
    size_t standIns = 0, copies = 0;
    for (const Spawned& sp : spawned) (sp.copyId >= 0 ? copies : standIns) += sp.hidden ? 0 : 1;
    GN_LOG("layouts: unloaded (%d objects put back, %llu standalone copies back in their group prop's place, %llu copies removed)", n,
           static_cast<unsigned long long>(standIns), static_cast<unsigned long long>(copies));
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
    if (s.entities.empty()) return;
    // It takes the place of the group prop it was made from, wherever that is in the selection.
    auto from = [&](const Selected& x) { return x.group && dist2(x.orig.trans, orig.trans) <= kMatch * kMatch; };
    if (g_haveSel && from(g_sel)) {
        g_sel = s;
    } else {
        bool replaced = false;
        for (Selected& e : g_extra)
            if (!replaced && from(e)) e = s, replaced = true;
        if (!replaced) {
            g_extra.clear();
            g_sel = s;
            g_haveSel = true;
        }
    }
    g_status = b + countNoteLocked();
}

// Client thread, after a copy was placed: select it for editing (with `add`, alongside the selection).
void selectCopy(const Spawned& sp, const gn_transform& now, bool add) {
    Selected s;
    s.copyId = sp.copyId;
    for (uintptr_t e : {sp.server, sp.client}) {
        const reflect::Type* t = e ? registry().typeOf(mem::ProcessMemory(), e) : nullptr;
        if (t) s.entities.push_back({e, t});
        if (t && s.kind.empty()) s.kind = kindOf(t->name);
    }
    s.orig = s.now = now;
    memcpy(s.at, sp.sourceAt, sizeof(s.at));
    char b[160];
    snprintf(b, sizeof(b), "Placed a copy (%s). Move it as usual; \"Remove this copy\" takes it away.", s.kind.c_str());
    std::lock_guard<std::mutex> lock(g_selM);
    if (s.entities.empty()) return;
    setPickedLocked(s, add);
    g_status = b + countNoteLocked();
}

// Server thread: a ray from the camera along its view; the object whose collision it hits.
// `ray` (from, direction; 6 floats) or, if null, the camera's view.
// With `add` (Shift held), what is picked joins the selection, or leaves it if it is already in.
void pick(const float* ray = nullptr, bool add = false) {
    std::array<float, 6> given{};
    const bool haveRay = ray != nullptr;
    if (ray) memcpy(given.data(), ray, sizeof(float) * 6);
    {
        // A new pick replaces the selection, even when it finds nothing to select: otherwise the
        // editor keeps showing (and moving) the previous object under a "not supported" message.
        std::lock_guard<std::mutex> lock(g_selM);
        if (!add) g_haveSel = false, g_extra.clear();
        g_status = "Picking...";
    }
    server().post([haveRay, given, add] {
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
            setPickedLocked(s, add);
            g_status = b + countNoteLocked();
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
                if (sp.server == entity && sp.copyId >= 0) { // a copy: its edit is found by its id
                    s.spawned = false;
                    s.copyId = sp.copyId;
                    s.orig = s.now;
                    memcpy(s.at, sp.sourceAt, sizeof(s.at));
                } else if (sp.server == entity) {
                    s.spawned = true;
                    s.orig = sp.orig;
                    memcpy(s.at, sp.orig.trans, sizeof(s.at));
                }
        }
        if (s.copyId >= 0) { // both halves of the copy, not whatever else is at the same place
            s.entities.clear();
            std::lock_guard<std::mutex> lock(g_spawnM);
            for (const Spawned& sp : g_spawned)
                if (sp.copyId == s.copyId) {
                    if (sp.serverType) s.entities.push_back({sp.server, sp.serverType});
                    if (sp.client && sp.clientType) s.entities.push_back({sp.client, sp.clientType});
                }
        } else {
            collectPair(s, s.now.trans);
        }
        char b[160];
        snprintf(b, sizeof(b), "Picked %s (%llu entities), hit %.1f m away%s", s.kind.c_str(),
                 static_cast<unsigned long long>(s.entities.size()), hit.distance,
                 guessed ? " (a guess: the nearest object to the hit)" : "");
        std::lock_guard<std::mutex> lock(g_selM);
        setPickedLocked(s, add);
        g_status = b + countNoteLocked();
    });
}

// Moves the selection to `to` (both realms) and records the edit.
// The kind an edit is filed under: standalone copies of group props are "GroupProp", keyed by the
// copy's original position in its group.
std::string editKind(const Selected& s) { return s.spawned ? "GroupProp" : s.kind; }

// ---- undo -------------------------------------------------------------------------------------------
// Each change to the layout leaves a step: a move (the object's placement and its edit before), a copy
// placed (taken away again) or a copy removed (brought back). Moves of one object in quick
// succession (a held key, a drag) are one step.
struct UndoStep {
    enum Kind { Move, Placed, Removed } kind = Move;
    Selected target;          // Move: the object, `now` = where it was
    bool hadEdit = false;     // Move: whether it had an edit, and the edit as it was
    Edit edit;                // Move: that edit; Removed: the copy's edit
    int copyId = -1;          // Placed / Removed
    uintptr_t server = 0;     // Removed: the parked copy's server entity
    double time = 0;
    int batch = 0;            // steps made together (a multiple selection) are undone together
};
std::mutex g_undoM;
std::vector<UndoStep> g_undo; // (g_undoM)
int g_undoBatch = 0;          // (g_undoM)
constexpr size_t kMaxUndo = 400;

// Steps made together. Moves of the same objects in quick succession (a held key, a drag) are one step.
void pushUndoBatch(std::vector<UndoStep> steps) {
    if (steps.empty()) return;
    const double now = static_cast<double>(GetTickCount64()) / 1000.0;
    std::lock_guard<std::mutex> lock(g_undoM);
    bool allMoves = true;
    for (const UndoStep& st : steps) allMoves = allMoves && st.kind == UndoStep::Move;
    if (allMoves && g_undo.size() >= steps.size()) {
        const size_t base = g_undo.size() - steps.size();
        const int b = g_undo[base].batch;
        bool same = base == 0 || g_undo[base - 1].batch != b;
        for (size_t i = 0; same && i < steps.size(); ++i) {
            const UndoStep& last = g_undo[base + i];
            same = last.kind == UndoStep::Move && last.batch == b && now - last.time < 0.7 && sameObject(last.target, steps[i].target);
        }
        if (same) { // the same move continuing: keep where it started
            for (size_t i = 0; i < steps.size(); ++i) g_undo[base + i].time = now;
            return;
        }
    }
    const int b = ++g_undoBatch;
    for (UndoStep& st : steps) {
        st.time = now;
        st.batch = b;
        g_undo.push_back(std::move(st));
    }
    while (g_undo.size() > kMaxUndo) g_undo.erase(g_undo.begin());
}

void clearUndo() {
    std::lock_guard<std::mutex> lock(g_undoM);
    g_undo.clear();
}

size_t undoCount() {
    std::lock_guard<std::mutex> lock(g_undoM);
    size_t n = 0;
    for (size_t i = 0; i < g_undo.size(); ++i) n += i == 0 || g_undo[i].batch != g_undo[i - 1].batch;
    return n;
}

// The selection's edit as it is now (g_m held).
Edit* editOf(const Selected& s) {
    for (Edit& ed : g_edits) {
        if (s.copyId >= 0 ? (ed.kind == "Copy" && ed.copyId == s.copyId)
                          : (ed.kind == editKind(s) && dist2(ed.at, s.at) <= kMatch * kMatch))
            return &ed;
    }
    return nullptr;
}

UndoStep moveStep(const Selected& s) {
    UndoStep st;
    st.kind = UndoStep::Move;
    st.target = s;
    std::lock_guard<std::mutex> lock(g_m);
    if (const Edit* ed = editOf(s)) st.hadEdit = true, st.edit = *ed;
    return st;
}

// One object to `to` (both realms), and its edit.
void moveObject(Selected s, const gn_transform& to) {
    s.now = to;
    if (s.group) {
        const GroupProp gp = s.gp;
        server().post([gp, to] { moveGroupProp(gp, to); });
        return;
    }
    for (const auto& [p, t] : s.entities) queueMove(p, t, to);
    if (s.copyId >= 0) {
        std::lock_guard<std::mutex> lock(g_m);
        for (Edit& ed : g_edits)
            if (ed.kind == "Copy" && ed.copyId == s.copyId) ed.to = ed.from = to, g_dirty = true;
        return;
    }
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

// `t` carried along by a change of `o` into `n` (as if fixed to it): the rest of a multiple
// selection moves and turns with the leading object, about its position.
gn_transform carry(const gn_transform& o, const gn_transform& n, const gn_transform& t) {
    const float* orows[3] = {o.right, o.up, o.forward};
    const float* nrows[3] = {n.right, n.up, n.forward};
    auto map = [&](const float* v, float* out) {
        float r[3] = {0.f, 0.f, 0.f};
        for (int i = 0; i < 3; ++i) {
            const float l2 = orows[i][0] * orows[i][0] + orows[i][1] * orows[i][1] + orows[i][2] * orows[i][2];
            if (l2 < 1e-12f) continue;
            const float c = (v[0] * orows[i][0] + v[1] * orows[i][1] + v[2] * orows[i][2]) / l2;
            for (int k = 0; k < 3; ++k) r[k] += c * nrows[i][k];
        }
        out[0] = r[0], out[1] = r[1], out[2] = r[2];
    };
    gn_transform out = t;
    map(t.right, out.right);
    map(t.up, out.up);
    map(t.forward, out.forward);
    const float d[3] = {t.trans[0] - o.trans[0], t.trans[1] - o.trans[1], t.trans[2] - o.trans[2]};
    float m[3];
    map(d, m);
    for (int k = 0; k < 3; ++k) out.trans[k] = n.trans[k] + m[k];
    return out;
}

// Moves the selection: the leading object to `to`, the rest of it along (records the edits). Group
// props are not moved (they are made standalone first).
void moveSelection(const gn_transform& to, bool record = true) {
    std::vector<std::pair<Selected, gn_transform>> moves; // object as it was, where it goes
    {
        std::lock_guard<std::mutex> lock(g_selM);
        if (!g_haveSel) return;
        const gn_transform from = g_sel.now;
        moves.push_back({g_sel, to});
        g_sel.now = to;
        for (Selected& e : g_extra) {
            const gn_transform next = carry(from, to, e.now);
            if (!e.group) moves.push_back({e, next});
            e.now = next;
        }
    }
    if (moves.size() > 1 && moves[0].first.group) moves.erase(moves.begin()); // the lead is a group prop: only the rest move
    if (record) {
        std::vector<UndoStep> steps;
        for (const auto& [obj, where] : moves) steps.push_back(moveStep(obj));
        pushUndoBatch(std::move(steps));
    }
    for (const auto& [obj, where] : moves) moveObject(obj, where);
}

// A copy is taken away: parked below the level (entities cannot be destroyed yet) for reuse.
void hideCopy(int copyId) {
    std::lock_guard<std::mutex> lock(g_spawnM);
    for (Spawned& sp : g_spawned)
        if (sp.copyId == copyId && !sp.hidden) {
            gn_transform away = sp.orig;
            if (sp.serverType && registry().typeOf(mem::ProcessMemory(), sp.server) == sp.serverType)
                entityTransform(reinterpret_cast<void*>(sp.server), away);
            away.trans[1] -= 15000.f;
            if (sp.serverType) queueMove(sp.server, sp.serverType, away);
            if (sp.client && sp.clientType) queueMove(sp.client, sp.clientType, away);
            sp.hidden = true;
            sp.copyId = -1;
        }
}

// One object put back (a copy: taken away); its undo step. False when there is nothing to undo.
bool resetObject(const Selected& s, UndoStep& st) {
    if (s.copyId >= 0) {
        st = UndoStep{};
        st.kind = UndoStep::Removed;
        st.copyId = s.copyId;
        for (const auto& [p, t] : s.entities)
            if (isServerClass(t->name)) st.server = p;
        {
            std::lock_guard<std::mutex> lock(g_m);
            if (const Edit* ed = editOf(s)) st.edit = *ed;
        }
        hideCopy(s.copyId);
        {
            std::lock_guard<std::mutex> lock(g_m);
            g_edits.erase(std::remove_if(g_edits.begin(), g_edits.end(), [&](const Edit& ed) { return ed.kind == "Copy" && ed.copyId == s.copyId; }),
                          g_edits.end());
            g_dirty = true;
        }
        return st.server && st.edit.kind == "Copy";
    }
    st = moveStep(s);
    const gn_transform back = s.orig;
    if (s.group) {
        const GroupProp gp = s.gp;
        server().post([gp, back] { moveGroupProp(gp, back); });
    }
    for (const auto& [p, t] : s.entities) queueMove(p, t, back);
    GN_LOG("layouts: %s put back at (%.2f, %.2f, %.2f) (%llu entities)", s.kind.c_str(), back.trans[0], back.trans[1], back.trans[2],
           static_cast<unsigned long long>(s.entities.size()));
    const std::string key = editKind(s);
    std::lock_guard<std::mutex> lock(g_m);
    for (size_t i = 0; i < g_edits.size(); ++i)
        if (g_edits[i].kind == key && dist2(g_edits[i].at, s.at) <= kMatch * kMatch) {
            g_edits.erase(g_edits.begin() + static_cast<long>(i));
            g_dirty = true;
            break;
        }
    return true;
}

// Everything selected put back where the level had it; copies taken away.
void resetSelection() {
    const std::vector<Selected> all = selectionAll();
    if (all.empty()) return;
    std::vector<UndoStep> steps;
    int removed = 0;
    for (const Selected& s : all) {
        UndoStep st;
        if (resetObject(s, st)) steps.push_back(std::move(st));
        removed += s.copyId >= 0;
    }
    pushUndoBatch(std::move(steps));
    std::lock_guard<std::mutex> lock(g_selM);
    std::vector<Selected> keep; // copies leave the selection; the rest stay, back in place
    for (Selected s : all)
        if (s.copyId < 0) s.now = s.orig, keep.push_back(s);
    g_haveSel = !keep.empty();
    if (g_haveSel) g_sel = keep[0], g_extra.assign(keep.begin() + 1, keep.end());
    else g_extra.clear();
    g_status = removed && removed == static_cast<int>(all.size()) ? (removed == 1 ? "Removed the copy." : "Removed the copies.")
             : removed ? "Put back; copies removed."
                       : "Put back where the level had it.";
}

// Takes back one step; a moved object (as it was) is added to `moved`.
void undoStep(const UndoStep& st, std::vector<Selected>& moved, bool& addSel) {
    if (st.kind == UndoStep::Placed) { // take the copy away
        hideCopy(st.copyId);
        {
            std::lock_guard<std::mutex> lock(g_m);
            g_edits.erase(std::remove_if(g_edits.begin(), g_edits.end(), [&](const Edit& ed) { return ed.kind == "Copy" && ed.copyId == st.copyId; }),
                          g_edits.end());
            g_dirty = true;
        }
        std::lock_guard<std::mutex> lock(g_selM);
        if (g_haveSel && g_sel.copyId == st.copyId) {
            if (g_extra.empty()) g_haveSel = false;
            else g_sel = g_extra.back(), g_extra.pop_back();
        }
        g_extra.erase(std::remove_if(g_extra.begin(), g_extra.end(), [&](const Selected& e) { return e.copyId == st.copyId; }), g_extra.end());
        g_status = "Undone: the copy placed last is gone.";
        return;
    }
    if (st.kind == UndoStep::Removed) { // bring the parked copy back
        Spawned back{};
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_spawnM);
            for (Spawned& sp : g_spawned)
                if (sp.server == st.server && sp.hidden && sp.serverType && registry().typeOf(mem::ProcessMemory(), sp.server) == sp.serverType) {
                    sp.hidden = false;
                    sp.copyId = st.copyId;
                    if (sp.serverType) queueMove(sp.server, sp.serverType, st.edit.to);
                    if (sp.client && sp.clientType) queueMove(sp.client, sp.clientType, st.edit.to);
                    back = sp;
                    found = true;
                    break;
                }
        }
        if (!found) return setStatus("Undo: that copy is gone (its part of the level was unloaded).");
        {
            std::lock_guard<std::mutex> lock(g_m);
            g_edits.push_back(st.edit);
            g_dirty = true;
        }
        selectCopy(back, st.edit.to, addSel);
        addSel = true;
        setStatus("Undone: the removed copy is back.");
        return;
    }
    // A move: the object back where it was, and its edit as it was.
    const Selected& s = st.target;
    if (s.group) {
        const GroupProp gp = s.gp;
        const gn_transform to = s.now;
        server().post([gp, to] { moveGroupProp(gp, to); });
    } else {
        for (const auto& [p, t] : s.entities) queueMove(p, t, s.now);
    }
    {
        std::lock_guard<std::mutex> lock(g_m);
        Edit* ed = editOf(s);
        if (st.hadEdit && ed) *ed = st.edit;
        else if (st.hadEdit) g_edits.push_back(st.edit);
        else if (ed) g_edits.erase(g_edits.begin() + (ed - g_edits.data()));
        g_dirty = true;
    }
    moved.push_back(s);
}

// Takes back the last step (all of it, when it moved several objects at once).
void undo() {
    std::vector<UndoStep> steps;
    {
        std::lock_guard<std::mutex> lock(g_undoM);
        if (g_undo.empty()) return setStatus("Nothing to undo.");
        const int b = g_undo.back().batch;
        while (!g_undo.empty() && g_undo.back().batch == b) {
            steps.push_back(g_undo.back());
            g_undo.pop_back();
        }
    }
    std::reverse(steps.begin(), steps.end()); // as they were made: the leading object first
    std::vector<Selected> moved;
    bool addSel = false;
    for (const UndoStep& st : steps) undoStep(st, moved, addSel);
    if (moved.empty()) return;
    std::lock_guard<std::mutex> lock(g_selM);
    g_sel = moved[0]; // the moved objects are selected again, where they were
    g_extra.assign(moved.begin() + 1, moved.end());
    g_haveSel = true;
    g_status = moved.size() == 1 ? "Undone: " + moved[0].kind + " is back where it was."
                                 : "Undone: " + std::to_string(moved.size()) + " objects are back where they were.";
}

// ---- copy and paste ----------------------------------------------------------------------------------
Template g_clip;          // (g_selM)
bool g_haveClip = false;  // (g_selM)

// The template for the selection (server thread).
bool templateOf(const Selected& s, Template& t) {
    t = Template{};
    if (s.copyId >= 0 || s.spawned) {
        std::lock_guard<std::mutex> lock(g_spawnM);
        for (const Spawned& sp : g_spawned) {
            const bool match = s.copyId >= 0 ? sp.copyId == s.copyId
                                             : (sp.copyId < 0 && !s.entities.empty() && (sp.server == s.entities[0].first || sp.client == s.entities[0].first));
            if (!match || !sp.data) continue;
            t.data = sp.data, t.serverBus = sp.serverBus, t.clientBus = sp.clientBus;
            setOwners(t, sp.ownerServer, sp.ownerClient);
            if (sp.copyId >= 0) t.source = sp.source, memcpy(t.at, sp.sourceAt, sizeof(t.at));
            else t.source = "GroupProp", memcpy(t.at, sp.orig.trans, sizeof(t.at));
            t.place = s.now;
            return true;
        }
        return false;
    }
    if (s.group) {
        t.data = memberType(s.gp.member);
        t.serverBus = busOf(s.gp.serverGroup);
        t.clientBus = busOf(s.gp.clientGroup);
        t.source = "GroupProp";
        memcpy(t.at, s.at, sizeof(t.at));
        t.place = s.now;
        setOwners(t, s.gp.serverGroup, s.gp.clientGroup);
        return t.data && t.serverBus && t.clientBus;
    }
    return resolveSource(s.kind, s.at, t);
}

std::string describe(const Template& t, const Selected& s) {
    const reflect::Type* dt = t.data ? registry().typeOf(mem::ProcessMemory(), t.data) : nullptr;
    char b[160];
    snprintf(b, sizeof(b), "%s (%s) from (%.1f, %.1f, %.1f)", s.group || s.spawned ? "group prop" : s.kind.c_str(), dt ? dt->name.c_str() : "?",
             t.at[0], t.at[1], t.at[2]);
    return b;
}

// Records a new copy in the layout (its id).
int addCopyEdit(const Template& t, const gn_transform& to) {
    std::lock_guard<std::mutex> lock(g_m);
    Edit ed;
    ed.kind = "Copy";
    ed.source = t.source;
    memcpy(ed.at, t.at, sizeof(ed.at));
    ed.to = ed.from = to;
    ed.haveFrom = true;
    ed.copyId = g_nextCopyId++;
    g_edits.push_back(ed);
    g_dirty = true;
    return ed.copyId;
}

bool selection(Selected& s) {
    std::lock_guard<std::mutex> lock(g_selM);
    s = g_sel;
    return g_haveSel;
}

std::vector<Template> g_clipAll; // (g_selM) everything copied, the leading object first

// Copy: the selection becomes the templates that Paste places (several keep where they are from
// each other).
void copySelection() {
    const std::vector<Selected> all = selectionAll();
    if (all.empty()) return setStatus("Copy: pick an object first");
    server().post([all] {
        std::vector<Template> ts;
        for (const Selected& s : all) {
            Template t;
            if (!templateOf(s, t)) continue;
            t.label = describe(t, s);
            ts.push_back(t);
        }
        if (ts.empty()) return setStatus("Copy: GridNode cannot make copies of this object (only static models and group props)");
        const size_t skipped = all.size() - ts.size();
        std::lock_guard<std::mutex> lock(g_selM);
        g_clip = ts[0];
        if (ts.size() > 1) g_clip.label = std::to_string(ts.size()) + " objects, from " + ts[0].label;
        g_clipAll = ts;
        g_haveClip = true;
        g_status = "Copied " + g_clip.label + ". Paste places new ones where the crosshair points." +
                   (skipped ? " (" + std::to_string(skipped) + " could not be copied.)" : std::string());
    });
}

// Places copies of `ts` (the first at `lead`, the rest where they were from it) as one undo step,
// selected together.
void placeCopies(const std::vector<Template>& ts, const std::vector<gn_transform>& places) {
    std::vector<UndoStep> steps;
    std::vector<int> ids;
    for (size_t i = 0; i < ts.size(); ++i) {
        const int id = addCopyEdit(ts[i], places[i]);
        UndoStep st;
        st.kind = UndoStep::Placed;
        st.copyId = id;
        steps.push_back(st);
        ids.push_back(id);
    }
    pushUndoBatch(std::move(steps));
    for (size_t i = 0; i < ts.size(); ++i) spawnCopy(ts[i], places[i], ids[i], true, i > 0);
}

// Paste: new copies of what was copied where the ray (from, direction) hits, or 10 m along it.
void pasteAt(const float* ray) {
    std::array<float, 6> r{};
    memcpy(r.data(), ray, sizeof(float) * 6);
    server().post([r] {
        std::vector<Template> ts;
        {
            std::lock_guard<std::mutex> lock(g_selM);
            if (!g_haveClip || g_clipAll.empty()) return void(g_status = "Paste: copy an object first");
            ts = g_clipAll;
        }
        const float to[3] = {r[0] + r[3] * 200.f, r[1] + r[4] * 200.f, r[2] + r[5] * 200.f};
        gn_ray_hit hit{};
        float lead[3];
        if (raycastObject(r.data(), to, &hit, nullptr, nullptr, nullptr) == GN_OK)
            memcpy(lead, hit.position, sizeof(lead));
        else
            for (int k = 0; k < 3; ++k) lead[k] = r[k] + r[3 + k] * 10.f;
        std::vector<gn_transform> places;
        for (const Template& t : ts) {
            alignas(16) gn_transform place = t.place;
            for (int k = 0; k < 3; ++k) place.trans[k] = lead[k] + (t.place.trans[k] - ts[0].place.trans[k]);
            place.trans[3] = 1.f;
            places.push_back(place);
        }
        placeCopies(ts, places);
    });
}

// Duplicate: new copies of the selection in the same places (selected, ready to move).
void duplicateSelection() {
    const std::vector<Selected> all = selectionAll();
    if (all.empty()) return setStatus("Duplicate: pick an object first");
    server().post([all] {
        std::vector<Template> ts;
        std::vector<gn_transform> places;
        for (const Selected& s : all) {
            Template t;
            if (!templateOf(s, t)) continue;
            ts.push_back(t);
            places.push_back(s.now);
        }
        if (ts.empty()) return setStatus("Duplicate: GridNode cannot make copies of this object (only static models and group props)");
        placeCopies(ts, places);
    });
}

// Turns the object about a world axis through its position (its rows keep their lengths, so scale stays).
void rotateAbout(gn_transform& t, const float* axisIn, float degrees) {
    float k[3] = {axisIn[0], axisIn[1], axisIn[2]};
    const float n = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
    if (n < 1e-6f) return;
    for (float& x : k) x /= n;
    const float a = degrees * 3.14159265f / 180.f, c = std::cos(a), s = std::sin(a);
    auto rot = [&](float* v) { // Rodrigues
        const float kv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
        const float cx = k[1] * v[2] - k[2] * v[1], cy = k[2] * v[0] - k[0] * v[2], cz = k[0] * v[1] - k[1] * v[0];
        const float o[3] = {v[0] * c + cx * s + k[0] * kv * (1.f - c), v[1] * c + cy * s + k[1] * kv * (1.f - c),
                            v[2] * c + cz * s + k[2] * kv * (1.f - c)};
        v[0] = o[0], v[1] = o[1], v[2] = o[2];
    };
    rot(t.right);
    rot(t.up);
    rot(t.forward);
}

void yaw(gn_transform& t, float degrees) {
    const float Y[3] = {0.f, 1.f, 0.f};
    rotateAbout(t, Y, degrees);
}

// Stands the object upright again (its up row along world up), keeping the way it faces.
void upright(gn_transform& t) {
    auto len = [](const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    const float lr = len(t.right), lu = len(t.up), lf = len(t.forward);
    float f[3] = {t.forward[0], 0.f, t.forward[2]};
    if (f[0] * f[0] + f[2] * f[2] < 1e-6f) f[0] = -t.up[0], f[2] = -t.up[2]; // was facing straight up or down
    if (f[0] * f[0] + f[2] * f[2] < 1e-6f) f[2] = 1.f;
    const float n = std::sqrt(f[0] * f[0] + f[2] * f[2]);
    f[0] /= n, f[2] /= n;
    // right = up x forward with up = Y: (f.z, 0, -f.x)
    t.right[0] = f[2] * lr, t.right[1] = 0.f, t.right[2] = -f[0] * lr;
    t.up[0] = 0.f, t.up[1] = lu, t.up[2] = 0.f;
    t.forward[0] = f[0] * lf, t.forward[1] = 0.f, t.forward[2] = f[2] * lf;
}

} // namespace

// ---- host interface ----------------------------------------------------------------------------------
void start(const std::wstring& dir) {
    g_dir = dir + L"layouts\\";
    for (const char* cls : {"ServerStaticModelEntity", "ClientStaticModelEntity"})
        onCreated("gridnode", cls, &onCreatedModel, nullptr); // Pam* models derive from these
    for (const char* cls : {"ServerStaticModelGroupEntity", "ClientStaticModelGroupEntity"})
        onCreated("gridnode", cls, &onCreatedGroup, nullptr);
    loadSettings();
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
        std::lock_guard<std::mutex> lock(g_undoM);
        g_undo.clear();
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

// Server thread, every 2 s: copies (and standalone stand-ins) whose part of the level streamed out
// are gone with it; forget them, and place what the layout has again once its source is back.
// Looking for sources walks every loaded group, so it runs only when something changed (a copy was
// destroyed, or a group was created since the last look), and never more often than 25 times what
// the last look took, so the game keeps its frame rate while sources are away.
void maintainSpawned() {
    size_t dead = 0;
    {
        std::lock_guard<std::mutex> lock(g_spawnM);
        const auto alive = [](const Spawned& sp) {
            return sp.serverType && registry().typeOf(mem::ProcessMemory(), sp.server) == sp.serverType;
        };
        const size_t before = g_spawned.size();
        g_spawned.erase(std::remove_if(g_spawned.begin(), g_spawned.end(), [&](const Spawned& sp) { return !alive(sp); }), g_spawned.end());
        dead = before - g_spawned.size();
    }
    if (dead) GN_LOG("layouts: %llu copies were destroyed with their part of the level; they come back when it loads again",
                     static_cast<unsigned long long>(dead));
    static uint32_t seenGen = 0;
    static bool pending = false; // something is still missing
    static ULONGLONG nextLook = 0;
    const uint32_t gen = g_groupGen.load();
    if (dead) pending = true;
    if (gen != seenGen) pending = true;
    const ULONGLONG now = GetTickCount64();
    if (!pending || now < nextLook) return;
    std::vector<Edit> edits;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_level != levelName()) return;
        for (const Edit& ed : g_edits)
            if (ed.kind == "Copy" || ed.kind == "GroupProp") edits.push_back(ed);
    }
    if (edits.empty() || !localServerCharacter()) return;
    seenGen = gen;
    pending = false;
    const ULONGLONG t0 = GetTickCount64();
    applyGroupEdits(edits, true);
    const ULONGLONG took = GetTickCount64() - t0;
    nextLook = GetTickCount64() + std::max<ULONGLONG>(2000, took * 25);
    static int slow = 0;
    if (took > 100 && slow++ < 5) GN_LOG("layouts: looking for copies' sources took %llu ms; next look in %llu s at the earliest", took, (nextLook - GetTickCount64()) / 1000);
}

void serverTick() {
    runQueue(g_serverQ);
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last >= 2000) {
        last = now;
        maintainSpawned();
    }
}

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

// ---- free camera (render thread drives it; the active camera's frame update applies it) -----------
// While the menu is open the game gets no input, so Faith stands still. Each frame the view manager
// asks the active camera to update itself (camera vtable +0x68), which writes the camera's two
// transforms (+0x10 and +0x50), and then copies them into the view the renderer uses. GridNode gives
// the active camera object a copy of its vtable whose +0x68 entry calls the game's own update and then
// writes the free camera's transform: a pointer swap on that one object, no code is patched.
// Right mouse held: look; WASD: move; Q/E: down/up; Shift: faster; Ctrl: slower.
constexpr size_t kCamSlots = 160;     // entries copied; the camera classes have well under that
constexpr size_t kCamFrameSlot = 0x68 / 8;
constexpr size_t kCamOutputSlot = 0x70 / 8; // computeTransforms(camera, info, LinearTransform out[2]) (MECMP)
uintptr_t g_camFakeVtbl[kCamSlots + 1]; // [0] is the RTTI slot in front of the table
uintptr_t g_camObject = 0;           // the camera object wearing the copy (client thread)
uintptr_t g_camOrigVtbl = 0;         // its own vtable
using CamFrameFn = void (*)(void*, void*);
CamFrameFn g_camOrigFrame = nullptr;
using CamOutputFn = void (*)(void*, void*, void*);
CamOutputFn g_camOrigOutput = nullptr;

std::mutex g_camM;
bool g_camApply = false;             // (g_camM)
gn_transform g_camT{};               // (g_camM) what the camera update writes

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
bool g_pointer = false;              // render thread: with the menu closed the mouse moves a cursor (right button: look)
bool g_gizmoHot = false;             // render thread: the mouse is on (or dragging) a handle, so a click does not pick
bool g_gizmoShown = false;           // render thread: the handles are showing (last frame)
int g_gizmoMode = 0;                 // render thread: 0 move, 1 rotate
bool g_gizmoLocal = true;            // render thread: handles along the object's own axes (else the world's)
int g_editorFrame = -10;             // last frame the Layout editor was drawn (render thread)

uintptr_t firstViewManager() {
    namespace cv = e::st::ClientCameraViewManagers::off;
    const uintptr_t managers = mem::readOr<uintptr_t>(mem::rva(e::global::ClientCameraViewManagers));
    const uintptr_t begin = managers ? mem::readOr<uintptr_t>(managers + cv::managersBegin) : 0;
    const uintptr_t end = managers ? mem::readOr<uintptr_t>(managers + cv::managersEnd) : 0;
    return begin && end > begin ? mem::readOr<uintptr_t>(begin) : 0;
}

std::atomic<unsigned> g_camCalls{0}, g_camOutputs{0}; // diagnostics: how often the game ran the camera's update / output

bool takeCamT(gn_transform& t) {
    std::lock_guard<std::mutex> lock(g_camM);
    if (!g_camApply) return false;
    t = g_camT;
    return true;
}

// The camera's transform output: everything that asks the camera where it is (its own update, and
// whatever else calls this slot) gets the free camera.
void camOutput(void* self, void* info, void* out) {
    g_camOutputs.fetch_add(1, std::memory_order_relaxed);
    if (g_camOrigOutput) g_camOrigOutput(self, info, out);
    alignas(16) gn_transform t{};
    if (!out || !takeCamT(t)) return;
    auto* o = static_cast<gn_transform*>(out);
    o[0] = t;
    o[1] = t;
}

void camFrameUpdate(void* self, void* info) {
    g_camCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_camOrigFrame) g_camOrigFrame(self, info);
    alignas(16) gn_transform t{};
    {
        std::lock_guard<std::mutex> lock(g_camM);
        if (!g_camApply) return;
        t = g_camT;
    }
    const uintptr_t cam = reinterpret_cast<uintptr_t>(self);
    namespace c = e::st::CameraObject::off;
    mem::write(cam + c::transform, &t, sizeof(t));
    mem::write(cam + c::viewTransform, &t, sizeof(t));
}

void unhookCamera() {
    if (g_camObject && mem::readOr<uintptr_t>(g_camObject) == reinterpret_cast<uintptr_t>(&g_camFakeVtbl[1]))
        mem::write<uintptr_t>(g_camObject, g_camOrigVtbl);
    g_camObject = 0;
}

// Client thread: put the copy on the view manager's active camera (again after it changes).
void hookCamera() {
    const uintptr_t mgr = firstViewManager();
    const uintptr_t cam = mgr ? mem::readOr<uintptr_t>(mgr + e::st::ClientCameraViewManager::off::activeCamera) : 0;
    const uintptr_t fake = reinterpret_cast<uintptr_t>(&g_camFakeVtbl[1]);
    if (!cam) return;
    const uintptr_t vt = mem::readOr<uintptr_t>(cam);
    if (vt == fake) return;
    if (cam != g_camObject) unhookCamera();
    if (!mem::inImage(vt)) return;
    if (vt != g_camOrigVtbl) {
        // A new camera class: build its copy (only while no object wears the old one).
        for (size_t k = 0; k <= kCamSlots; ++k) g_camFakeVtbl[k] = mem::readOr<uintptr_t>(vt - 8 + 8 * k);
        g_camOrigVtbl = vt;
        g_camOrigFrame = reinterpret_cast<CamFrameFn>(g_camFakeVtbl[1 + kCamFrameSlot]);
        g_camFakeVtbl[1 + kCamFrameSlot] = reinterpret_cast<uintptr_t>(&camFrameUpdate);
        g_camOrigOutput = reinterpret_cast<CamOutputFn>(g_camFakeVtbl[1 + kCamOutputSlot]);
        if (mem::inImage(reinterpret_cast<uintptr_t>(g_camOrigOutput)))
            g_camFakeVtbl[1 + kCamOutputSlot] = reinterpret_cast<uintptr_t>(&camOutput);
        else
            g_camOrigOutput = nullptr;
        GN_LOG("layouts: free camera drives camera %llx (vtable %llx)", static_cast<unsigned long long>(cam),
               static_cast<unsigned long long>(vt));
    }
    if (!g_camOrigFrame) return;
    mem::write<uintptr_t>(cam, fake);
    g_camObject = cam;
}


// While the free camera is on, Faith shows the third-person body (with her head) instead of the
// first-person one: the same per-object vtable copy, on the character's view-mode interface, whose
// getViewMode answers 1 (MECMP found the slot and the meaning). Its other entries are the originals.
constexpr size_t kBodySlots = 96;
uintptr_t g_bodyFakeVtbl[kBodySlots + 1];
uintptr_t g_bodyIface = 0, g_bodyOrigVtbl = 0; // client thread

int bodyViewMode(void*) { return 1; }

void unhookBody() {
    if (g_bodyIface && mem::readOr<uintptr_t>(g_bodyIface) == reinterpret_cast<uintptr_t>(&g_bodyFakeVtbl[1]))
        mem::write<uintptr_t>(g_bodyIface, g_bodyOrigVtbl);
    g_bodyIface = 0;
}

void hookBody() {
    namespace c = e::constant;
    const auto character = reinterpret_cast<uintptr_t>(localClientCharacter());
    const uintptr_t iface = character ? character + c::ClientCharacter_ViewModeInterface : 0;
    const uintptr_t fake = reinterpret_cast<uintptr_t>(&g_bodyFakeVtbl[1]);
    if (!iface) return;
    const uintptr_t vt = mem::readOr<uintptr_t>(iface);
    if (vt == fake) return;
    if (iface != g_bodyIface) unhookBody();
    const uintptr_t slotAt = mem::rva(c::ViewModeSlot);
    if (!mem::inImage(vt) || slotAt < vt || slotAt >= vt + 8 * kBodySlots) { // not the interface we know
        static bool said = false;
        if (!said) GN_LOG("layouts: free camera: Faith's view-mode interface has vtable %llx, not the known one; keeping her first-person body",
                          static_cast<unsigned long long>(vt));
        said = true;
        return;
    }
    const size_t slot = (slotAt - vt) / 8;
    for (size_t k = 0; k <= kBodySlots; ++k) g_bodyFakeVtbl[k] = mem::readOr<uintptr_t>(vt - 8 + 8 * k);
    g_bodyFakeVtbl[1 + slot] = reinterpret_cast<uintptr_t>(&bodyViewMode);
    g_bodyOrigVtbl = vt;
    mem::write<uintptr_t>(iface, fake);
    g_bodyIface = iface;
    GN_LOG("layouts: free camera: Faith shows her third-person body (view-mode slot %u)", static_cast<unsigned>(slot));
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
    // On whether or not the menu is open; with it closed the mouse always looks and the game gets no input.
    const bool want = g_cam.enabled && localClientCharacter() != nullptr;
    if (want && !g_cam.active) {
        if (!startFreeCam()) return;
        g_cam.active = true;
        client().post([] { hookCamera(); hookBody(); });
        GN_LOG("layouts: free camera on at (%.1f, %.1f, %.1f)", g_cam.pos[0], g_cam.pos[1], g_cam.pos[2]);
    } else if (!want && g_cam.active) {
        g_cam.active = false;
        g_cam.looking = false;
        g_pointer = false;
        ui::setPointer(false);
        ui::setLookCapture(false);
        ui::setInputCapture(false);
        {
            std::lock_guard<std::mutex> lock(g_camM);
            g_camApply = false;
        }
        client().post([] { unhookCamera(); unhookBody(); });
        GN_LOG("layouts: free camera off");
    }
    if (!g_cam.active) return;
    if (!menuOpen) { // the keys, top left, while flying with the menu closed
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float pad = ImGui::GetFontSize();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + pad, vp->Pos.y + pad), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("##freecam", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextUnformatted("Free camera");
        ImGui::Separator();
        auto row = [](const std::string& key, const char* what) {
            ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "%s", key.c_str());
            ImGui::SameLine(ImGui::GetFontSize() * 7.5f);
            ImGui::TextUnformatted(what);
        };
        auto k = [](int i) { return g_binds[i].vk ? ui::keyName(g_binds[i].vk) : std::string("-"); };
        row("Mouse", g_pointer ? "pointer (right button: look)" : "look");
        row("W A S D", "fly");
        row("Q / E", "down / up");
        row("Shift / Ctrl", "faster / slower");
        row("Wheel", "flying speed");
        ImGui::Separator();
        row("Click / " + k(kPick), "pick at the crosshair");
        row("Shift + click", "add to / take from the selection");
        row(k(kPointer), g_pointer ? "mouse: back to looking" : "mouse: a pointer, to drag the handles");
        row(k(kHandles), g_gizmoMode ? "handles: rotate (now) / move" : "handles: move (now) / rotate");
        row(k(kFwd) + " " + k(kBack) + " " + k(kLeft) + " " + k(kRight), "move it");
        row(k(kUp) + " / " + k(kDown), "move it up / down");
        row(k(kTurnL) + " / " + k(kTurnR), "turn it");
        row(k(kTiltL) + " / " + k(kTiltR), "tilt it left / right");
        row(k(kTipAway) + " / " + k(kTipToward), "tip it away / toward");
        row(k(kFlip), "flip it upside down");
        row(k(kUpright), "stand it upright");
        row("Ctrl+" + k(kUndo), "undo");
        row(k(kCopy), "copy");
        row(k(kPaste), "paste at the crosshair");
        row(k(kDuplicate), "duplicate in place");
        ImGui::Separator();
        row(k(kTeleport), "teleport Faith here (keep flying)");
        row("Shift+" + k(kTeleport), "teleport Faith here and go back to her");
        row(k(kFreeCam), "back to Faith");
        Selected cur;
        if (selection(cur)) {
            ImGui::Separator();
            const size_t n = selectionAll().size();
            ImGui::Text("Selected: %s", cur.group ? "a group prop (made standalone when you move it)"
                                       : cur.copyId >= 0 ? (cur.kind + " (copy)").c_str() : cur.kind.c_str());
            if (n > 1) ImGui::Text("and %llu more (they move and turn with it)", static_cast<unsigned long long>(n - 1));
        }
        ImGui::End();
    }
    if (!menuOpen) { // which way the mouse works, at the bottom of the screen
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        auto key = [](int i) { return g_binds[i].vk ? ui::keyName(g_binds[i].vk) : std::string("-"); };
        Selected cur;
        const bool sel = selection(cur);
        std::string text = g_pointer ? "POINTER   " + key(kPointer) + ": back to looking   right mouse: look"
                                     : "LOOKING   " + key(kPointer) + ": pointer" + (sel ? ", to drag the handles" : "");
        if (sel && g_pointer) text += std::string("   ") + key(kHandles) + ": handles " + (g_gizmoMode ? "rotate" : "move");
        if (sel && cur.group) text += "   (group prop: made standalone when you move it)";
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        const ImVec2 at(vp->Pos.x + (vp->Size.x - size.x) * 0.5f, vp->Pos.y + vp->Size.y - size.y - ImGui::GetFontSize() * 2.f);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float pad = ImGui::GetFontSize() * 0.4f;
        dl->AddRectFilled(ImVec2(at.x - pad, at.y - pad), ImVec2(at.x + size.x + pad, at.y + size.y + pad), IM_COL32(0, 0, 0, 140), pad);
        dl->AddText(at, g_pointer ? IM_COL32(255, 220, 110, 255) : IM_COL32(230, 230, 230, 230), text.c_str());
    }
    if (ImGui::GetFrameCount() % 60 == 0) client().post([] { hookCamera(); hookBody(); });

    ImGuiIO& io = ImGui::GetIO();
    ui::setInputCapture(!menuOpen);
    ui::setPointer(!menuOpen && g_pointer);
    if (!menuOpen && !g_pointer) {
        g_cam.looking = true;
    } else if (!menuOpen) { // pointer mode: like the menu, the right button looks
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !g_gizmoHot) g_cam.looking = true;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) g_cam.looking = false;
    } else {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !io.WantCaptureMouse) g_cam.looking = true;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) g_cam.looking = false;
    }
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
        if (io.MouseWheel != 0.f && (g_cam.looking || (!menuOpen && g_pointer))) g_cam.speed = std::clamp(g_cam.speed * (io.MouseWheel > 0 ? 1.25f : 0.8f), 0.5f, 200.f);
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

// The current view's look direction, screen right and up, and position (free camera or the game's).
bool viewBasis(float* from, float* d, float* r, float* u) {
    if (g_cam.active) {
        camBasis(g_cam, d, r, u);
        memcpy(from, g_cam.pos, sizeof(float) * 3);
    } else {
        gn_transform v{};
        if (!cameraTransform(v)) return false;
        for (int k = 0; k < 3; ++k) d[k] = -v.forward[k], r[k] = v.right[k], u[k] = v.up[k], from[k] = v.trans[k];
        normalize(d), normalize(r), normalize(u);
    }
    return true;
}

// The ray through a screen point (nx, ny in -1..1, y up), from the current view.
bool viewRay(float nx, float ny, float* ray) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x < 1.f || io.DisplaySize.y < 1.f) return false;
    float from[3], d[3], r[3], u[3];
    if (!viewBasis(from, d, r, u)) return false;
    const uintptr_t mgr = firstViewManager();
    const float fovDeg = mgr ? mem::readOr<float>(mgr + e::st::ClientCameraViewManager::off::fovYDegrees) : 0.f;
    const float fov = (fovDeg > 10.f && fovDeg < 170.f ? fovDeg : 70.f) * 3.14159265f / 180.f;
    const float aspect = io.DisplaySize.x / io.DisplaySize.y;
    const float th = std::tan(fov * 0.5f);
    for (int k = 0; k < 3; ++k) ray[3 + k] = d[k] + r[k] * nx * th * aspect + u[k] * ny * th;
    normalize(ray + 3);
    memcpy(ray, from, sizeof(from));
    return true;
}

bool cursorRay(float* ray) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x < 1.f || io.DisplaySize.y < 1.f) return false;
    return viewRay(2.f * io.MousePos.x / io.DisplaySize.x - 1.f, 1.f - 2.f * io.MousePos.y / io.DisplaySize.y, ray);
}
bool centreRay(float* ray) { return viewRay(0.f, 0.f, ray); }

void pickCentre(bool add = false) {
    float ray[6];
    if (centreRay(ray)) pick(ray, add);
    else pick(nullptr, add);
}
// ---- teleports (the game's own: ServerCharacterEntity_teleport, as its teleport entities use) ------
// Faith at `pos`, facing along `facing` (horizontal). Server thread does the move.
void teleportFaith(const float* pos, const float* facing, const std::string& what) {
    float f[3] = {facing[0], 0.f, facing[2]};
    if (f[0] * f[0] + f[2] * f[2] < 1e-6f) f[2] = 1.f;
    normalize(f);
    alignas(16) gn_transform t{};
    t.up[1] = 1.f;
    t.forward[0] = f[0], t.forward[2] = f[2];
    t.right[0] = f[2], t.right[2] = -f[0]; // up x forward
    memcpy(t.trans, pos, sizeof(float) * 3);
    t.trans[3] = 1.f;
    server().post([t, what] {
        void* faith = localServerCharacter();
        const int r = faith ? teleport(faith, &t) : GN_ERR_NOT_FOUND;
        GN_LOG("layouts: teleport to %s (%.2f, %.2f, %.2f): %s", what.c_str(), t.trans[0], t.trans[1], t.trans[2], r == GN_OK ? "done" : "failed");
        setStatus(r == GN_OK ? "Teleported Faith to " + what + "." : "Teleport failed (no character, or the game's teleport is not as expected).");
    });
}

// Faith where the crosshair points (on what it hits, or at the camera when it hits nothing within
// 200 m), facing the way the camera looks; then the view goes back to her.
// With `keepFlying`, the free camera stays where it is (only Faith moves).
void teleportCrosshair(bool keepFlying = false) {
    float ray[6];
    if (!centreRay(ray)) return setStatus("Teleport: no camera");
    std::array<float, 6> r{};
    memcpy(r.data(), ray, sizeof(ray));
    if (!keepFlying) g_cam.enabled = false; // back to Faith's view
    server().post([r] {
        const float to[3] = {r[0] + r[3] * 200.f, r[1] + r[4] * 200.f, r[2] + r[5] * 200.f};
        gn_ray_hit hit{};
        float pos[3] = {r[0], r[1], r[2]};
        const bool onSomething = raycastObject(r.data(), to, &hit, nullptr, nullptr, nullptr) == GN_OK;
        if (onSomething) {
            // A little back from the surface and up, so she stands on it rather than in it.
            for (int k = 0; k < 3; ++k) pos[k] = hit.position[k] - r[3 + k] * 0.3f;
            pos[1] = std::max(pos[1], hit.position[1]) + 0.05f;
        }
        const float facing[3] = {r[3], 0.f, r[5]};
        teleportFaith(pos, facing, onSomething ? "the crosshair" : "the camera");
    });
}

// Teleport points: named places per level (layouts\<level>\teleports.txt: name|x|y|z|yaw).
struct TeleportPoint {
    std::string name;
    float pos[3] = {};
    float yaw = 0.f; // radians, atan2(forward.x, forward.z)
};

std::wstring teleportFile(const std::string& level) { return levelDir(level) + L"teleports.txt"; }

std::vector<TeleportPoint> loadTeleports(const std::string& level) {
    std::vector<TeleportPoint> out;
    FILE* f = _wfopen(teleportFile(level).c_str(), L"rb");
    if (!f) return out;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        const size_t bar = l.find('|');
        if (bar == std::string::npos || l[0] == '#') continue;
        TeleportPoint p;
        p.name = l.substr(0, bar);
        if (sscanf(l.c_str() + bar + 1, "%f|%f|%f|%f", &p.pos[0], &p.pos[1], &p.pos[2], &p.yaw) >= 3) out.push_back(p);
    }
    fclose(f);
    return out;
}

void saveTeleports(const std::string& level, const std::vector<TeleportPoint>& pts) {
    CreateDirectoryW(g_dir.c_str(), nullptr);
    CreateDirectoryW(levelDir(level).c_str(), nullptr);
    std::string o = "# GridNode teleport points: name|x|y|z|yaw (radians)\n";
    for (const TeleportPoint& p : pts) {
        char b[256];
        snprintf(b, sizeof(b), "%s|%.3f|%.3f|%.3f|%.4f\n", p.name.c_str(), p.pos[0], p.pos[1], p.pos[2], p.yaw);
        o += b;
    }
    writeLine(teleportFile(level), o);
}

void pasteCentre() {
    float ray[6];
    if (centreRay(ray)) pasteAt(ray);
    else setStatus("Paste: no camera");
}

// Moves the selection one step relative to the view: dir 0 away, 1 toward, 2 left, 3 right, 4 up, 5 down;
// 6 / 7 turn left / right. Shift: 4 steps, Ctrl: a quarter.
void nudge(int dir, float scale) {
    Selected s;
    if (!selection(s)) return;
    if (s.group) { // a group prop leads: it becomes standalone first (the next press moves it)
        static double askedAt = -10.0;
        if (ImGui::GetTime() - askedAt > 1.0) askedAt = ImGui::GetTime(), standaloneGroupProps();
        return;
    }
    float from[3] = {}, d[3] = {}, r[3] = {}, u[3] = {};
    if ((dir < 4 || (dir >= 8 && dir <= 12)) && !viewBasis(from, d, r, u)) return;
    gn_transform t = s.now;
    float f[3] = {d[0], 0.f, d[2]}, side[3] = {r[0], 0.f, r[2]}; // the view's directions along the ground
    if (dir < 4 || (dir >= 8 && dir <= 12)) normalize(f), normalize(side);
    if (dir >= 8) { // 8 / 9 tilt left / right, 10 / 11 tip away / toward, 12 upside down, 13 upright
        if (dir == 8 || dir == 9) rotateAbout(t, f, (dir == 8 ? -1.f : 1.f) * g_turnStep * scale);
        else if (dir == 10 || dir == 11) rotateAbout(t, side, (dir == 10 ? 1.f : -1.f) * g_turnStep * scale);
        else if (dir == 12) rotateAbout(t, f, 180.f);
        else upright(t);
    } else if (dir >= 6) {
        yaw(t, (dir == 6 ? 1.f : -1.f) * g_turnStep * scale);
    } else if (dir >= 4) {
        t.trans[1] += (dir == 4 ? 1.f : -1.f) * g_nudge * scale;
    } else { // along the ground
        const float* axis = dir < 2 ? f : side;
        const float sign = dir == 0 || dir == 3 ? 1.f : -1.f;
        for (int k = 0; k < 3; ++k) t.trans[k] += axis[k] * sign * g_nudge * scale;
    }
    moveSelection(t);
}

void drawKeys();

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
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Close the menu to fly: the mouse looks, WASD move, Q/E down/up, Shift faster, Ctrl slower, wheel: speed. With the menu open, hold right mouse to look and click objects to pick them.");
        ImGui::PopTextWrapPos();
    }
    ImGui::TextDisabled("Left-click an object in the world to pick it.");
    ImGui::SeparatorText("Teleport");
    {
        static std::vector<TeleportPoint> points;
        static std::string pointsFor;
        if (pointsFor != level) points = level.empty() ? std::vector<TeleportPoint>{} : loadTeleports(level), pointsFor = level;
        ImGui::BeginDisabled(!g_cam.active);
        if (ImGui::Button("Teleport Faith to the crosshair")) teleportCrosshair(true);
        ImGui::SameLine();
        if (ImGui::Button("... and go back to her")) teleportCrosshair(false);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(g_cam.active ? "(%s while flying)" : "(fly the free camera to use it; %s)", ui::keyName(g_binds[kTeleport].vk).c_str());
        static char pname[48] = "";
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        ImGui::InputTextWithHint("##pname", "name", pname, sizeof(pname));
        ImGui::SameLine();
        ImGui::BeginDisabled(level.empty() || !localClientCharacter());
        if (ImGui::Button("Save Faith's position")) {
            gn_transform ft{};
            if (void* faith = localClientCharacter(); faith && entityTransform(faith, ft)) {
                TeleportPoint p;
                p.name = *pname ? std::string(pname) : "point " + std::to_string(points.size() + 1);
                for (char& c : p.name)
                    if (c == '|') c = '/';
                memcpy(p.pos, ft.trans, sizeof(p.pos));
                p.yaw = std::atan2(ft.forward[0], ft.forward[2]);
                points.push_back(p);
                saveTeleports(level, points);
                pname[0] = 0;
            }
        }
        ImGui::EndDisabled();
        int remove = -1;
        for (size_t i = 0; i < points.size(); ++i) {
            ImGui::PushID(static_cast<int>(i) + 7000);
            if (ImGui::SmallButton("Go")) {
                const float facing[3] = {std::sin(points[i].yaw), 0.f, std::cos(points[i].yaw)};
                g_cam.enabled = false;
                teleportFaith(points[i].pos, facing, "\"" + points[i].name + "\"");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove = static_cast<int>(i);
            ImGui::SameLine();
            ImGui::Text("%s", points[i].name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(%.0f, %.0f, %.0f)", points[i].pos[0], points[i].pos[1], points[i].pos[2]);
            ImGui::PopID();
        }
        if (remove >= 0) {
            points.erase(points.begin() + remove);
            saveTeleports(level, points);
        }
        if (points.empty()) ImGui::TextDisabled("No teleport points saved for this level.");
    }
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
        saveSettings();
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
    if (ImGui::Button("Pick what the camera points at")) pickCentre();
    ImGui::SameLine();
    const size_t steps = undoCount();
    ImGui::BeginDisabled(steps == 0);
    if (ImGui::Button("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%llu step%s", static_cast<unsigned long long>(steps), steps == 1 ? "" : "s");
    Selected s;
    bool have, haveClip;
    std::string status, clip;
    {
        std::lock_guard<std::mutex> lock(g_selM);
        s = g_sel;
        have = g_haveSel;
        status = g_status;
        haveClip = g_haveClip;
        clip = g_clip.label;
    }
    if (!status.empty()) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("%s", status.c_str());
        ImGui::PopTextWrapPos();
    }
    if (have && s.group) {
        ImGui::TextWrapped("This prop is part of a static model group, which the game draws as one batch. Make it standalone to move it: "
                           "the group's copy is hidden and a standalone copy takes its place. Copy and Duplicate work on it as it is.");
        if (ImGui::Button("Make standalone")) {
            const GroupProp gp = s.gp;
            const gn_transform at = s.orig;
            setStatus("Making a standalone copy...");
            server().post([gp, at] { makeStandalone(gp, at, at, true); });
        }
    } else if (have) {
        ImGui::Text("%s%s  (%llu entities)", s.kind.c_str(),
                    s.copyId >= 0 ? " (a copy)" : s.spawned ? " (standalone copy of a group prop)" : "",
                    static_cast<unsigned long long>(s.entities.size()));
        gn_transform t = s.now;
        bool changed = ImGui::DragFloat3("Position", t.trans, 0.05f, 0.f, 0.f, "%.2f");
        if (changed) moveSelection(t);
        // Steps relative to the view.
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        if (ImGui::InputFloat("##nudge", &g_nudge, 0.f, 0.f, "%.2f m")) g_nudge = std::clamp(g_nudge, 0.01f, 100.f), saveSettings();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        if (ImGui::InputFloat("##turn", &g_turnStep, 0.f, 0.f, "%.0f deg")) g_turnStep = std::clamp(g_turnStep, 0.1f, 180.f), saveSettings();
        const float scale = ImGui::GetIO().KeyShift ? 4.f : ImGui::GetIO().KeyCtrl ? 0.25f : 1.f;
        if (ImGui::Button("Away")) nudge(0, scale);
        ImGui::SameLine();
        if (ImGui::Button("Closer")) nudge(1, scale);
        ImGui::SameLine();
        if (ImGui::Button("Left")) nudge(2, scale);
        ImGui::SameLine();
        if (ImGui::Button("Right")) nudge(3, scale);
        ImGui::SameLine();
        if (ImGui::Button("Up")) nudge(4, scale);
        ImGui::SameLine();
        if (ImGui::Button("Down")) nudge(5, scale);
        if (ImGui::Button("Turn left")) nudge(6, scale);
        ImGui::SameLine();
        if (ImGui::Button("Turn right")) nudge(7, scale);
        if (ImGui::Button("Tilt left")) nudge(8, scale);
        ImGui::SameLine();
        if (ImGui::Button("Tilt right")) nudge(9, scale);
        ImGui::SameLine();
        if (ImGui::Button("Tip away")) nudge(10, scale);
        ImGui::SameLine();
        if (ImGui::Button("Tip toward")) nudge(11, scale);
        if (ImGui::Button("Flip upside down")) nudge(12, 1.f);
        ImGui::SameLine();
        if (ImGui::Button("Stand upright")) nudge(13, 1.f);
        ImGui::TextDisabled("Directions follow the camera. Shift: 4 steps, Ctrl: a quarter.");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Handles:");
        ImGui::SameLine();
        ImGui::RadioButton("move", &g_gizmoMode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("rotate", &g_gizmoMode, 1);
        ImGui::SameLine();
        ImGui::Checkbox("its own axes", &g_gizmoLocal);
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Drag the arrows or rings on the selection (Ctrl snaps to the steps above). While flying with the menu closed, "
                            "%s gives the mouse a pointer for them; %s switches move / rotate.",
                            ui::keyName(g_binds[kPointer].vk).c_str(), ui::keyName(g_binds[kHandles].vk).c_str());
        ImGui::PopTextWrapPos();
        const size_t selected = selectionAll().size();
        if (selected > 1) ImGui::TextDisabled("%llu objects selected (Shift+click adds or takes away); they move and turn with this one.",
                                              static_cast<unsigned long long>(selected));
        if (ImGui::Button(selected > 1 ? "Put them all back (copies: remove)" : s.copyId >= 0 ? "Remove this copy" : "Put back where the level had it"))
            resetSelection();
        if (s.copyId < 0) ImGui::TextDisabled("level position (%.2f, %.2f, %.2f)", s.at[0], s.at[1], s.at[2]);
    } else {
        ImGui::TextDisabled("Point the camera at an object and press Pick.");
    }
    // Copies.
    ImGui::BeginDisabled(!have);
    if (ImGui::Button("Copy")) copySelection();
    ImGui::SameLine();
    if (ImGui::Button("Duplicate in place")) duplicateSelection();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!haveClip);
    if (ImGui::Button("Paste at the crosshair")) pasteCentre();
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0.f);
    if (haveClip) ImGui::TextDisabled("Copied: %s", clip.c_str());
    else ImGui::TextDisabled("Copy an object (a building, a prop) to paste new ones of it anywhere in this level. Copies are saved with the layout.");
    ImGui::PopTextWrapPos();
    drawKeys();
    ImGui::End();
}

// ---- key binds (read with GetAsyncKeyState so they work with the menu closed) ----------------------------
int g_rebinding = -1;            // the bind waiting for a key (render thread)
bool g_keyWas[kBindCount] = {};
double g_keyNext[kBindCount] = {};

bool keyHeld(int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000) != 0; }

// A press of bind `i` this frame (with repeats while held, for binds that repeat).
bool bindPressed(int i) {
    const bool down = keyHeld(g_binds[i].vk);
    const bool was = g_keyWas[i];
    g_keyWas[i] = down;
    if (!down) return false;
    const double now = ImGui::GetTime();
    if (!was) {
        g_keyNext[i] = now + 0.35;
        return true;
    }
    if (g_binds[i].repeat && now >= g_keyNext[i]) {
        g_keyNext[i] = now + 0.06;
        return true;
    }
    return false;
}

void drawKeys() {
    if (!ImGui::CollapsingHeader("Keys")) return;
    ImGui::TextDisabled("Free-camera keys work only while the free camera is on.");
    for (int i = 0; i < kBindCount; ++i) {
        ImGui::PushID(i);
        const std::string name = g_rebinding == i ? std::string("press a key...") : g_binds[i].vk ? ui::keyName(g_binds[i].vk) : std::string("none");
        if (ImGui::Button(name.c_str(), ImVec2(ImGui::GetFontSize() * 7, 0))) {
            g_rebinding = i;
            for (int vk = 0x04; vk <= 0xFE; ++vk) GetAsyncKeyState(vk); // forget presses from before
        }
        ImGui::SameLine();
        ImGui::Text("%s%s", g_binds[i].label, g_binds[i].freeCamOnly ? "" : "  (always)");
        ImGui::PopID();
    }
    if (g_rebinding >= 0) ImGui::TextDisabled("Escape: no key. Backspace: the default.");
    if (ImGui::SmallButton("All keys back to the defaults")) {
        for (Bind& b : g_binds) b.vk = b.def;
        saveSettings();
    }
}

// While rebinding: the first key pressed (mouse buttons other than the side ones are left alone).
void rebindFrame() {
    if (g_rebinding < 0) return;
    for (int vk = 0x04; vk <= 0xFE; ++vk) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= 0xA0 && vk <= 0xA5)) continue;
        if (!(GetAsyncKeyState(vk) & 1)) continue; // pressed since the last call
        Bind& b = g_binds[g_rebinding];
        b.vk = vk == VK_ESCAPE ? 0 : vk == VK_BACK ? b.def : vk;
        g_rebinding = -1;
        for (int i = 0; i < kBindCount; ++i) g_keyWas[i] = true; // no action for the key just pressed
        saveSettings();
        return;
    }
}

void keysFrame(bool menuOpen) {
    const ImGuiIO& io = ImGui::GetIO();
    if (g_rebinding >= 0) return rebindFrame();
    if (!ui::gameFocused() || io.WantTextInput) {
        for (bool& w : g_keyWas) w = true; // nothing fires when focus comes back with a key held
        return;
    }
    if (bindPressed(kFreeCam) && localClientCharacter()) g_cam.enabled = !g_cam.enabled;
    const bool flying = g_cam.active;
    bool fire[kBindCount] = {};
    for (int i = kPick; i < kBindCount; ++i) fire[i] = bindPressed(i);
    // Undo: Ctrl + its key, while flying or with the menu open. With Ctrl held the key does nothing else.
    if (io.KeyCtrl || keyHeld(VK_CONTROL)) {
        if (fire[kUndo] && (flying || menuOpen)) undo();
        for (int i = kPick; i < kBindCount; ++i)
            if (g_binds[i].vk == g_binds[kUndo].vk) fire[i] = false;
    }
    fire[kUndo] = false;
    if (!flying) return;
    const bool adding = io.KeyShift || keyHeld(VK_SHIFT);
    if (fire[kPick]) pickCentre(adding);
    if (fire[kCopy]) copySelection();
    if (fire[kPaste]) pasteCentre();
    if (fire[kDuplicate]) duplicateSelection();
    // The camera stays (switching back to Faith's view as she teleports makes the level reload around
    // her, which takes copies with it); Shift: back to her view, as before.
    if (fire[kTeleport]) teleportCrosshair(!(io.KeyShift || keyHeld(VK_SHIFT)));
    if (fire[kPointer]) g_pointer = !g_pointer;
    if (fire[kHandles]) g_gizmoMode ^= 1;
    const float scale = io.KeyShift ? 4.f : io.KeyCtrl ? 0.25f : 1.f;
    for (int i = kFwd; i <= kUpright; ++i)
        if (fire[i]) nudge(i - kFwd, scale);
    // With the menu closed: a left click picks what the crosshair points at, and the crosshair shows.
    if (!menuOpen && g_pointer) { // the cursor picks, as with the menu open
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !g_gizmoHot) {
            float ray[6];
            if (cursorRay(ray)) pick(ray, adding);
        }
    } else if (!menuOpen) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !g_gizmoHot) pickCentre(adding);
        const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float k = ImGui::GetFontSize() * 0.5f;
        dl->AddLine(ImVec2(c.x - k, c.y), ImVec2(c.x + k, c.y), IM_COL32(255, 255, 255, 200), 1.5f);
        dl->AddLine(ImVec2(c.x, c.y - k), ImVec2(c.x, c.y + k), IM_COL32(255, 255, 255, 200), 1.5f);
    }
}

// ---- selection outline ------------------------------------------------------------------------------
// The selected object's box: the union of its physics parts' boxes from its StaticModelEntityData
// (PhysicsPartInfos: PartInfoData {Aabb, Translation}), in the object's own frame, turned and placed
// with it and drawn through the current view.
uintptr_t selectionData(const Selected& s) {
    if (s.group) return memberType(s.gp.member);
    if (s.copyId >= 0 || s.spawned) {
        std::lock_guard<std::mutex> lock(g_spawnM);
        for (const Spawned& sp : g_spawned)
            if (s.copyId >= 0 ? sp.copyId == s.copyId : (!s.entities.empty() && (sp.server == s.entities[0].first || sp.client == s.entities[0].first)))
                return sp.data;
    }
    for (const auto& [p, t] : s.entities)
        if (const uintptr_t d = dataOf(p, "StaticModelEntityData")) return d;
    return 0;
}

// A group prop's box, from the group's collision: the compound shape's instance tree (+0xc0) is a
// Havok static tree with 6-byte compressed nodes (hknpStaticCompoundShape's box update, 0x143e99090):
// node array at tree +0x10 (count +0x18), the root's box at +0x20 (min) / +0x30 (max). A node's
// bytes 0..2 give its box inside its parent's, per axis: min = parent min + (byte >> 4)^2 * step,
// max = parent max - (byte & 15)^2 * step, step = parent size * a constant (0x1425a8e10). Byte 3
// bit 7 marks an inner node: its left child is the next node, its right child (byte 3 & 0x7f) << 16 |
// u16 at +4, times two, nodes on; a leaf's u16 at +4 is its instance. Boxes are in the compound's
// space (the world: groups sit at the origin).
bool instanceWorldBox(uintptr_t body, int instance, float* mn, float* mx) {
    namespace c = e::constant;
    const uintptr_t shape = body ? mem::readOr<uintptr_t>(body + c::GroupRigidBody_Shape) : 0;
    const uintptr_t tree = shape ? mem::readOr<uintptr_t>(shape + c::CompoundShape_Tree) : 0;
    const uintptr_t nodes = tree ? mem::readOr<uintptr_t>(tree + c::CompoundTree_Nodes) : 0;
    const int32_t count = tree ? mem::readOr<int32_t>(tree + c::CompoundTree_NodeCount) : 0;
    if (!nodes || count <= 0 || count > 4000000 || instance < 0) return false;
    float k = mem::readOr<float>(mem::rva(e::global::HavokAabb6Step), 0.f);
    if (!(k > 0.f && k < 0.05f)) k = 1.f / 226.f;
    struct Item {
        int32_t index;
        float mn[3], mx[3];
    };
    Item stack[96];
    int n = 0;
    stack[n].index = 0;
    if (!mem::read(tree + c::CompoundTree_Min, stack[n].mn, sizeof(float) * 3) || !mem::read(tree + c::CompoundTree_Max, stack[n].mx, sizeof(float) * 3))
        return false;
    ++n;
    auto child = [&](const Item& parent, int32_t index, Item& out) {
        uint8_t b[3];
        if (index < 0 || index >= count || !mem::read(nodes + static_cast<uintptr_t>(index) * 6, b, sizeof(b))) return false;
        out.index = index;
        for (int a = 0; a < 3; ++a) {
            const float step = (parent.mx[a] - parent.mn[a]) * k;
            const float hi = static_cast<float>(b[a] >> 4), lo = static_cast<float>(b[a] & 15);
            out.mn[a] = parent.mn[a] + hi * hi * step;
            out.mx[a] = parent.mx[a] - lo * lo * step;
        }
        return true;
    };
    for (int guard = 0; n > 0 && guard < 4000000; ++guard) {
        const Item it = stack[--n];
        const uintptr_t node = nodes + static_cast<uintptr_t>(it.index) * 6;
        const uint8_t flags = mem::readOr<uint8_t>(node + 3);
        const uint16_t data = mem::readOr<uint16_t>(node + 4);
        if (!(flags & 0x80)) { // a leaf
            if (data != instance) continue;
            memcpy(mn, it.mn, sizeof(it.mn));
            memcpy(mx, it.mx, sizeof(it.mx));
            return finite(mn, 3) && finite(mx, 3);
        }
        if (n + 2 > 96) return false;
        const int32_t right = it.index + static_cast<int32_t>((static_cast<uint32_t>(flags & 0x7f) << 16 | data) * 2);
        Item l, r;
        if (child(it, right, r)) stack[n++] = r;
        if (child(it, it.index + 1, l)) stack[n++] = l;
    }
    return false;
}

// A world box as a box in an object's own frame (the box around its corners there).
void localFromWorld(const gn_transform& t, const float* wmn, const float* wmx, float* mn, float* mx) {
    gn_transform I{};
    I.right[0] = I.up[1] = I.forward[2] = 1.f;
    for (int i = 0; i < 8; ++i) {
        const float w[3] = {i & 1 ? wmx[0] : wmn[0], i & 2 ? wmx[1] : wmn[1], i & 4 ? wmx[2] : wmn[2]};
        float l[3];
        reframe(t, I, w, true, l);
        for (int k = 0; k < 3; ++k) {
            mn[k] = i ? std::min(mn[k], l[k]) : l[k];
            mx[k] = i ? std::max(mx[k], l[k]) : l[k];
        }
    }
}

// Local boxes learnt from group props, by their StaticModelEntityData: a standalone copy (or any
// static model with that data) uses its prop's box. Render thread.
std::unordered_map<uintptr_t, std::array<float, 6>> g_dataBox;

// A mesh's local box, as the renderer culls with it: a RigidMeshEntity (render object) keeps its
// transform at +0x10, its world bounding sphere at +0x50 and its mesh's local box at
// *(**(object + 0x60) + 0x20) (min xyz_, max xyz_), as its set-transform (0x14376c060) reads it.
bool meshBox(uintptr_t ro, float* mn, float* mx) {
    const uintptr_t h = ro ? mem::readOr<uintptr_t>(ro + 0x60) : 0;
    const uintptr_t x = h ? mem::readOr<uintptr_t>(h) : 0;
    const uintptr_t p = x ? mem::readOr<uintptr_t>(x + 0x20) : 0;
    float b[8];
    if (!p || !mem::read(p, b, sizeof(b)) || !finite(b, 3) || !finite(b + 4, 3)) return false;
    for (int k = 0; k < 3; ++k)
        if (b[4 + k] < b[k] || b[4 + k] - b[k] > 5000.f) return false;
    memcpy(mn, b, sizeof(float) * 3);
    memcpy(mx, b + 4, sizeof(float) * 3);
    return true;
}

bool isMeshEntity(uintptr_t p) {
    if (p < 0x10000 || (p & 7)) return false;
    const uintptr_t vt = mem::readOr<uintptr_t>(p);
    if (vt == mem::rva(0x141bf72c8)) return true; // RigidMeshEntity (group props, most models)
    if (vt == mem::rva(0x141c012c0)) return true; // CompositeMeshEntity (some standalone models); same base and layout
    const reflect::Type* t = mem::inImage(vt) ? registry().typeOf(mem::ProcessMemory(), p) : nullptr;
    return t && t->name.size() > 10 && t->name.compare(t->name.size() - 10, 10, "MeshEntity") == 0;
}

// A render object's world transform (+0x10: right, up, forward, translation rows).
bool meshTransform(uintptr_t ro, gn_transform& t) {
    float m[16];
    if (!ro || !mem::read(ro + 0x10, m, sizeof(m)) || !finite(m, 16)) return false;
    for (int k = 0; k < 3; ++k) t.right[k] = m[k], t.up[k] = m[4 + k], t.forward[k] = m[8 + k], t.trans[k] = m[12 + k];
    return true;
}

// A mesh entity that is drawn at `where` (within 0.75 m): any other is some other object's.
bool meshAt(uintptr_t ro, const float* where) {
    gn_transform t{};
    return isMeshEntity(ro) && meshTransform(ro, t) && dist2(t.trans, where) < 0.5625f;
}

// A mesh entity drawn at `where`, reachable from [base, base + size): directly, or up to `depth`
// pointers down.
uintptr_t findMesh(uintptr_t base, uint32_t size, int depth, const float* where) {
    for (uint32_t off = 0; off < size; off += 8) {
        const uintptr_t p = mem::readOr<uintptr_t>(base + off);
        if (meshAt(p, where)) return p;
    }
    if (depth <= 0) return 0;
    for (uint32_t off = 0; off < size; off += 8) {
        const uintptr_t p = mem::readOr<uintptr_t>(base + off);
        if (p < 0x10000 || (p & 7) || mem::inImage(p)) continue;
        if (const uintptr_t q = findMesh(p, 0x180, depth - 1, where)) return q;
    }
    return 0;
}

// The render object that draws this selection, found by reading only (any thread): for a group
// prop, its member's render object list (the block's +0xc8, one per copy); for a standalone model,
// one its client entity points to. Only one drawn where the selection is counts.
uintptr_t drawnObjectOf(const Selected& s) {
    const float* at = s.now.trans;
    if (s.group) {
        if (!s.gp.block) return 0;
        const uintptr_t list = mem::readOr<uintptr_t>(s.gp.block + 0xc8);
        if (list > 0x10000 && !(list & 7)) {
            const uintptr_t ro = mem::readOr<uintptr_t>(list + s.gp.copy * 8u);
            if (meshAt(ro, at)) return ro;
        }
        return findMesh(s.gp.block, 0x130, 1, at);
    }
    for (const auto& [ent, ty] : s.entities) {
        if (isServerClass(ty->name)) continue;
        if (const uintptr_t m = findMesh(ent + 0x18, 0x2e8, 1, at)) return m;
        if (const uintptr_t m = findMesh(ent + 0x18, 0x2e8, 2, at)) return m;
    }
    static uintptr_t deepFor = 0; // once per selection: three levels down (slower)
    if (!s.entities.empty() && deepFor != s.entities[0].first) {
        deepFor = s.entities[0].first;
        for (const auto& [ent, ty] : s.entities) {
            if (isServerClass(ty->name)) continue;
            if (const uintptr_t m = findMesh(ent + 0x18, 0x2e8, 3, at)) return m;
            GN_LOG("layouts: outline: no render object found from %s %llx", ty->name.c_str(), static_cast<unsigned long long>(ent));
        }
    }
    return 0;
}

// The outline's render object asked of the game on the client thread (a group copy's comes from its
// physics component, a call), for the selection `key`.
std::mutex g_outAskM;
std::unordered_map<uintptr_t, uintptr_t> g_outAsk; // (g_outAskM) selection key -> render object (0: none yet)

bool localBox(uintptr_t data, const Selected& s, float* mn, float* mx, const char** from) {
    const auto it = data ? g_dataBox.find(data) : g_dataBox.end();
    if (it != g_dataBox.end()) {
        memcpy(mn, it->second.data(), sizeof(float) * 3);
        memcpy(mx, it->second.data() + 3, sizeof(float) * 3);
        return *from = "its model's mesh (seen before)", true;
    }
    // Until the mesh is found: the collision box (the group's tree), which can differ from what is drawn.
    if (s.group) {
        int index = -1;
        float wmn[3], wmx[3];
        if (groupInstance(s.gp.serverBody, s.gp.part, &index) && instanceWorldBox(s.gp.serverBody, index, wmn, wmx)) {
            localFromWorld(s.orig, wmn, wmx, mn, mx);
            return *from = "the collision, until the mesh is found", true;
        }
    }
    return false;
}

// World point -> screen; false behind the camera.
struct View {
    float from[3], d[3], r[3], u[3];
    float th = 1.f, aspect = 1.f, w = 1.f, h = 1.f;
};
bool currentView(View& v) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x < 1.f || !viewBasis(v.from, v.d, v.r, v.u)) return false;
    const uintptr_t mgr = firstViewManager();
    const float fovDeg = mgr ? mem::readOr<float>(mgr + e::st::ClientCameraViewManager::off::fovYDegrees) : 0.f;
    v.th = std::tan((fovDeg > 10.f && fovDeg < 170.f ? fovDeg : 70.f) * 3.14159265f / 360.f);
    v.aspect = io.DisplaySize.x / io.DisplaySize.y;
    v.w = io.DisplaySize.x, v.h = io.DisplaySize.y;
    return true;
}
constexpr float kNear = 0.05f;
float depthOf(const View& v, const float* p) {
    const float q[3] = {p[0] - v.from[0], p[1] - v.from[1], p[2] - v.from[2]};
    return dot3(q, v.d);
}
ImVec2 project(const View& v, const float* p) {
    const float q[3] = {p[0] - v.from[0], p[1] - v.from[1], p[2] - v.from[2]};
    const float z = std::max(dot3(q, v.d), kNear);
    const float x = dot3(q, v.r) / (z * v.th * v.aspect), y = dot3(q, v.u) / (z * v.th);
    return ImVec2((x + 1.f) * 0.5f * v.w, (1.f - y) * 0.5f * v.h);
}
// A 3D line, cut at the near plane.
void line3d(ImDrawList* dl, const View& v, const float* a, const float* b, ImU32 col, float thick) {
    float p[3] = {a[0], a[1], a[2]}, q[3] = {b[0], b[1], b[2]};
    const float za = depthOf(v, p), zb = depthOf(v, q);
    if (za < kNear && zb < kNear) return;
    if (za < kNear || zb < kNear) {
        const float t = (kNear - za) / (zb - za);
        float* cut = za < kNear ? p : q;
        for (int k = 0; k < 3; ++k) cut[k] = a[k] + (b[k] - a[k]) * t;
    }
    dl->AddLine(project(v, p), project(v, q), col, thick);
}

// The leading selection as drawn this frame, for the handles (render thread).
struct LeadFrame {
    bool valid = false;
    gn_transform t{};    // its drawn transform
    float centre[3] = {}; // the middle of its box, in the world
    bool group = false;
};
LeadFrame g_lead;

// One selected object's outline, looked up once and kept while it stays selected (render thread).
struct Outline {
    bool box = false;
    float mn[3] = {}, mx[3] = {};
    uintptr_t ro = 0;     // the render object that draws it, when found
    double boxAt = -10.0, roAt = -10.0;
    bool asked = false;   // asked the game for it (a group prop's copy)
};
std::unordered_map<uintptr_t, Outline> g_outlines;

uintptr_t outlineKey(const Selected& s) {
    return s.group ? s.gp.serverGroup ^ (uintptr_t(s.gp.part) << 3) : s.entities.empty() ? 0 : s.entities[0].first;
}

void drawOne(const View& v, const Selected& s, bool lead) {
    const uintptr_t key = outlineKey(s);
    const bool fresh = g_outlines.find(key) == g_outlines.end();
    Outline& o = g_outlines[key];
    const uintptr_t data = selectionData(s);
    const double now = ImGui::GetTime();
    // The box: its model's mesh once seen, else the collision; again every 2 s while there is none.
    if (fresh || (!o.box && now - o.boxAt > 2.0)) {
        o.boxAt = now;
        const char* from = "no box found";
        o.box = localBox(data, s, o.mn, o.mx, &from);
        if (fresh)
            GN_LOG("layouts: outline: %s, %s (%.2f %.2f %.2f) .. (%.2f %.2f %.2f)", s.kind.c_str(), from, o.mn[0], o.mn[1], o.mn[2], o.mx[0], o.mx[1],
                   o.mx[2]);
    }
    // Drawn in the render object's own transform when it is found (what is on screen).
    auto adopt = [&](uintptr_t found, const char* how) {
        o.ro = found;
        float rmn[3], rmx[3];
        if (meshBox(found, rmn, rmx)) {
            memcpy(o.mn, rmn, sizeof(rmn)), memcpy(o.mx, rmx, sizeof(rmx)), o.box = true;
            if (data) g_dataBox[data] = {rmn[0], rmn[1], rmn[2], rmx[0], rmx[1], rmx[2]};
        }
        GN_LOG("layouts: outline: render object %llx (%s), mesh box (%.2f %.2f %.2f) .. (%.2f %.2f %.2f)", (unsigned long long)found, how, o.mn[0],
               o.mn[1], o.mn[2], o.mx[0], o.mx[1], o.mx[2]);
    };
    if (fresh) {
        o.roAt = now;
        if (const uintptr_t found = drawnObjectOf(s)) {
            adopt(found, "read");
        } else if (s.group) {
            o.asked = true;
            {
                std::lock_guard<std::mutex> lock(g_outAskM);
                g_outAsk[key] = 0;
            }
            const GroupProp gp = s.gp;
            float at[3] = {s.now.trans[0], s.now.trans[1], s.now.trans[2]};
            client().post([gp, at, key] {
                const uintptr_t r = renderObjectOf(gp);
                std::lock_guard<std::mutex> lock(g_outAskM);
                const auto it = g_outAsk.find(key);
                if (it != g_outAsk.end() && meshAt(r, at)) it->second = r;
            });
        }
    } else if (!o.ro && o.asked) {
        uintptr_t got = 0;
        {
            std::lock_guard<std::mutex> lock(g_outAskM);
            const auto it = g_outAsk.find(key);
            if (it != g_outAsk.end() && it->second) got = it->second, g_outAsk.erase(it);
        }
        if (got) adopt(got, "asked the game"), o.asked = false;
    } else if (!o.ro && now - o.roAt > 2.0) {
        o.roAt = now;
        if (const uintptr_t found = drawnObjectOf(s)) adopt(found, "read");
    }
    gn_transform rt{};
    const gn_transform& t = o.ro && meshTransform(o.ro, rt) ? rt : s.now;
    auto world = [&](float x, float y, float z, float* out) {
        for (int k = 0; k < 3; ++k) out[k] = t.trans[k] + x * t.right[k] + y * t.up[k] + z * t.forward[k];
    };
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    // The leading object (what moves and turns the rest) yellow, the rest of the selection light blue.
    const ImU32 col = lead ? IM_COL32(255, 200, 60, 230) : IM_COL32(120, 200, 255, 210);
    if (o.box) {
        float c[8][3];
        for (int i = 0; i < 8; ++i) world(i & 1 ? o.mx[0] : o.mn[0], i & 2 ? o.mx[1] : o.mn[1], i & 4 ? o.mx[2] : o.mn[2], c[i]);
        static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& e : edges) line3d(dl, v, c[e[0]], c[e[1]], col, lead ? 2.f : 1.5f);
    }
    float pv[3];
    world(0, 0, 0, pv);
    if (depthOf(v, pv) > kNear) dl->AddCircleFilled(project(v, pv), lead ? 3.5f : 2.5f, col);
    if (!lead) return;
    {
        float mid[3] = {0, 0, 0};
        if (o.box)
            for (int k = 0; k < 3; ++k) mid[k] = 0.5f * (o.mn[k] + o.mx[k]);
        g_lead.valid = true;
        g_lead.t = t;
        g_lead.group = s.group;
        world(mid[0], mid[1], mid[2], g_lead.centre);
    }
    if (g_gizmoShown) return; // the handles are drawn instead of the axes
    // Its own axes from the middle of its box (its pivot, often a corner, is the small dot): right red,
    // up green, forward blue.
    float len = 1.f, c0[3] = {0, 0, 0};
    if (o.box) {
        len = std::max(0.5f, 0.35f * std::max({o.mx[0] - o.mn[0], o.mx[1] - o.mn[1], o.mx[2] - o.mn[2]}));
        for (int k = 0; k < 3; ++k) c0[k] = 0.5f * (o.mn[k] + o.mx[k]);
    }
    float org[3], a[3];
    auto axisEnd = [&](int k, float f, float* out) {
        world(c0[0] + (k == 0 ? len * f : 0), c0[1] + (k == 1 ? len * f : 0), c0[2] + (k == 2 ? len * f : 0), out);
    };
    world(c0[0], c0[1], c0[2], org);
    static const char* names[3] = {"X", "Y", "Z"};
    static const ImU32 cols[3] = {IM_COL32(240, 70, 70, 255), IM_COL32(80, 220, 80, 255), IM_COL32(80, 140, 255, 255)};
    for (int k = 0; k < 3; ++k) {
        axisEnd(k, 1.f, a);
        line3d(dl, v, org, a, cols[k], 2.f);
        axisEnd(k, 1.1f, a);
        if (depthOf(v, a) > kNear) dl->AddText(project(v, a), cols[k], names[k]);
    }
}

// The selection's outlines, shown while the free camera is on.
void drawOutline() {
    g_lead.valid = false;
    const std::vector<Selected> all = g_cam.enabled ? selectionAll() : std::vector<Selected>{};
    // Forget the outlines of what is no longer selected.
    for (auto it = g_outlines.begin(); it != g_outlines.end();) {
        bool keep = false;
        for (const Selected& s : all) keep = keep || outlineKey(s) == it->first;
        if (keep) {
            ++it;
        } else {
            std::lock_guard<std::mutex> lock(g_outAskM);
            g_outAsk.erase(it->first);
            it = g_outlines.erase(it);
        }
    }
    if (all.empty()) return;
    View v;
    if (!currentView(v)) return;
    for (size_t i = all.size(); i-- > 0;) drawOne(v, all[i], i == 0); // the lead last, on top
}


// ---- handles (move / rotate by dragging) -----------------------------------------------------------
// On the leading selection, from the middle of its box: three arrows (move along an axis) or three
// rings (turn about one), along its own axes or the world's. They are used with the cursor: with
// the menu open, or with the menu closed in pointer mode. A drag is one undo step; Ctrl snaps to
// the editor's steps. Everything selected follows (moveSelection).
struct GizmoDrag {
    int axis = -1;
    bool rotate = false;
    gn_transform start{};    // the leading selection's transform when the drag began
    float centre[3] = {};    // the handles' origin then
    float dir[3] = {};       // the axis (unit, world)
    float t0 = 0.f;          // move: where on the axis the drag began
    float a0 = 0.f;          // rotate: the cursor's screen angle about the centre then
    float sign = 1.f;        // rotate: screen angle -> rotation about dir
    bool first = true;
    float last = 0.f;        // the last value applied (a still mouse moves nothing)
    bool waiting = false;    // group props in the selection are being made standalone first
    double since = 0.0;
};
GizmoDrag g_drag;

// Closest point on the line (o, a) to the ray (f, d): the parameter along the line.
bool lineRayParam(const float* o, const float* a, const float* f, const float* d, float& t) {
    const float w[3] = {o[0] - f[0], o[1] - f[1], o[2] - f[2]};
    const float b = dot3(a, d), c = dot3(d, d), aa = dot3(a, a);
    const float den = aa * c - b * b;
    if (std::fabs(den) < 1e-6f) return false; // looking along the axis
    t = (b * dot3(d, w) - c * dot3(a, w)) / den;
    return std::isfinite(t);
}

float distToSegment(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float vx = b.x - a.x, vy = b.y - a.y, wx = p.x - a.x, wy = p.y - a.y;
    const float l2 = vx * vx + vy * vy;
    const float t = l2 > 0.f ? std::clamp((wx * vx + wy * vy) / l2, 0.f, 1.f) : 0.f;
    const float dx = a.x + t * vx - p.x, dy = a.y + t * vy - p.y;
    return std::sqrt(dx * dx + dy * dy);
}

void rotateVec(float* v, const float* axis, float degrees) {
    gn_transform tmp{};
    memcpy(tmp.right, v, sizeof(float) * 3);
    rotateAbout(tmp, axis, degrees);
    memcpy(v, tmp.right, sizeof(float) * 3);
}

void gizmoFrame(bool menuOpen) {
    g_gizmoHot = false;
    g_gizmoShown = false;
    const ImGuiIO& io = ImGui::GetIO();
    const bool cursor = menuOpen || g_pointer;
    if (!g_cam.active || !cursor || !g_lead.valid) {
        g_drag.axis = -1;
        return;
    }
    View v;
    if (!currentView(v)) return;
    const float* c = g_lead.centre; // the handles follow the object; a drag's own sums use where it began
    if (depthOf(v, c) < kNear * 4.f) return;
    g_gizmoShown = true;
    // Axes: the object's (its drawn rows) or the world's.
    float ax[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (g_gizmoLocal) {
        const float* rows[3] = {g_lead.t.right, g_lead.t.up, g_lead.t.forward};
        for (int k = 0; k < 3; ++k) {
            memcpy(ax[k], rows[k], sizeof(float) * 3);
            normalize(ax[k]);
        }
    }
    if (g_drag.axis >= 0) memcpy(ax[g_drag.axis], g_drag.dir, sizeof(float) * 3);
    const float dist = std::sqrt(std::max(dist2(v.from, c), 1e-4f));
    const float L = dist * v.th * 0.32f; // about a sixth of the screen's height
    const ImVec2 mouse = io.MousePos;
    const ImVec2 c2 = project(v, c);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    static const ImU32 cols[3] = {IM_COL32(240, 70, 70, 255), IM_COL32(80, 220, 80, 255), IM_COL32(80, 140, 255, 255)};
    const ImU32 hotCol = IM_COL32(255, 230, 90, 255);
    const bool overUi = menuOpen && io.WantCaptureMouse && g_drag.axis < 0;
    const bool rotate = g_drag.axis >= 0 ? g_drag.rotate : g_gizmoMode == 1;
    // Hover: the nearest handle within 9 px.
    int hover = -1;
    float best = 9.f;
    std::vector<ImVec2> ring[3];
    for (int k = 0; k < 3; ++k) {
        if (!rotate) {
            float e[3];
            for (int i = 0; i < 3; ++i) e[i] = c[i] + ax[k][i] * L;
            if (depthOf(v, e) < kNear) continue;
            const float d = distToSegment(mouse, c2, project(v, e));
            if (d < best) best = d, hover = k;
        } else {
            const int a = (k + 1) % 3, b = (k + 2) % 3; // a ring in the plane of the other two axes
            for (int i = 0; i <= 64; ++i) {
                const float th = 6.2831853f * static_cast<float>(i) / 64.f;
                float p[3];
                for (int j = 0; j < 3; ++j) p[j] = c[j] + (ax[a][j] * std::cos(th) + ax[b][j] * std::sin(th)) * L * 0.9f;
                ring[k].push_back(depthOf(v, p) > kNear ? project(v, p) : ImVec2(-1e9f, -1e9f));
            }
            for (size_t i = 1; i < ring[k].size(); ++i) {
                if (ring[k][i - 1].x < -1e8f || ring[k][i].x < -1e8f) continue;
                const float d = distToSegment(mouse, ring[k][i - 1], ring[k][i]);
                if (d < best) best = d, hover = k;
            }
        }
    }
    if (overUi || g_cam.looking) hover = -1;
    const int active = g_drag.axis >= 0 ? g_drag.axis : hover;
    g_gizmoHot = active >= 0;
    // Draw.
    for (int k = 0; k < 3; ++k) {
        const ImU32 col = k == active ? hotCol : cols[k];
        const float thick = k == active ? 4.f : 3.f;
        if (!rotate) {
            float e[3];
            for (int i = 0; i < 3; ++i) e[i] = c[i] + ax[k][i] * L;
            line3d(dl, v, c, e, col, thick);
            if (depthOf(v, e) > kNear) { // a head: a triangle across the arrow's end
                const ImVec2 p0 = c2, p1 = project(v, e);
                float dx = p1.x - p0.x, dy = p1.y - p0.y;
                const float n = std::sqrt(dx * dx + dy * dy);
                if (n > 1.f) {
                    dx /= n, dy /= n;
                    const float h = 12.f, w = 6.f;
                    dl->AddTriangleFilled(ImVec2(p1.x + dx * h, p1.y + dy * h), ImVec2(p1.x - dy * w, p1.y + dx * w), ImVec2(p1.x + dy * w, p1.y - dx * w), col);
                }
            }
        } else {
            for (size_t i = 1; i < ring[k].size(); ++i)
                if (ring[k][i - 1].x > -1e8f && ring[k][i].x > -1e8f) dl->AddLine(ring[k][i - 1], ring[k][i], col, thick);
        }
    }
    dl->AddCircleFilled(c2, 4.f, IM_COL32(255, 255, 255, 220));
    // Begin a drag.
    if (g_drag.axis < 0 && hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        Selected s;
        if (!selection(s)) return;
        g_drag = GizmoDrag{};
        g_drag.axis = hover;
        g_drag.rotate = rotate;
        g_drag.start = s.now;
        g_drag.waiting = standaloneGroupProps(); // a group prop cannot move as it is
        g_drag.since = ImGui::GetTime();
        memcpy(g_drag.centre, c, sizeof(g_drag.centre));
        memcpy(g_drag.dir, ax[hover], sizeof(g_drag.dir));
        float ray[6];
        if (!rotate) {
            if (!cursorRay(ray) || !lineRayParam(c, g_drag.dir, ray, ray + 3, g_drag.t0)) {
                g_drag.axis = -1;
                return;
            }
        } else {
            g_drag.a0 = std::atan2(-(mouse.y - c2.y), mouse.x - c2.x);
            // Which way a positive turn about the axis goes on screen: turn a point a little and look.
            const int a = (hover + 1) % 3;
            float p[3], q[3];
            for (int j = 0; j < 3; ++j) p[j] = ax[a][j] * L;
            memcpy(q, p, sizeof(q));
            rotateVec(q, g_drag.dir, 5.f);
            for (int j = 0; j < 3; ++j) p[j] += c[j], q[j] += c[j];
            const ImVec2 sp = project(v, p), sq = project(v, q);
            const float ap = std::atan2(-(sp.y - c2.y), sp.x - c2.x), aq = std::atan2(-(sq.y - c2.y), sq.x - c2.x);
            float da = aq - ap;
            while (da > 3.14159265f) da -= 6.2831853f;
            while (da < -3.14159265f) da += 6.2831853f;
            g_drag.sign = da >= 0.f ? 1.f : -1.f;
        }
        return;
    }
    if (g_drag.axis < 0) return;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { // released: done
        g_drag.axis = -1;
        return;
    }
    if (g_drag.waiting) { // until the standalone copies have taken the group props' places
        bool groups = false;
        for (const Selected& e : selectionAll()) groups = groups || e.group;
        Selected s;
        if (groups && ImGui::GetTime() - g_drag.since < 3.0) return;
        if (!selection(s) || s.group) { // it did not happen
            g_drag.axis = -1;
            return;
        }
        g_drag.waiting = false;
        g_drag.start = s.now;
    }
    // Drag: the leading selection's new transform (the rest follow it).
    gn_transform t = g_drag.start;
    float value = 0.f;
    if (!g_drag.rotate) {
        float ray[6], tp;
        if (!cursorRay(ray) || !lineRayParam(g_drag.centre, g_drag.dir, ray, ray + 3, tp)) return;
        value = tp - g_drag.t0;
        if (io.KeyCtrl && g_nudge > 0.f) value = std::round(value / g_nudge) * g_nudge;
        for (int k = 0; k < 3; ++k) t.trans[k] += g_drag.dir[k] * value;
    } else {
        const ImVec2 s2 = project(v, g_drag.centre);
        float a = std::atan2(-(mouse.y - s2.y), mouse.x - s2.x) - g_drag.a0;
        while (a > 3.14159265f) a -= 6.2831853f;
        while (a < -3.14159265f) a += 6.2831853f;
        value = a * 57.2957795f * g_drag.sign;
        if (io.KeyCtrl && g_turnStep > 0.f) value = std::round(value / g_turnStep) * g_turnStep;
        rotateAbout(t, g_drag.dir, value);
        float off[3] = {t.trans[0] - g_drag.centre[0], t.trans[1] - g_drag.centre[1], t.trans[2] - g_drag.centre[2]};
        rotateVec(off, g_drag.dir, value); // about the middle of the box, not the pivot
        for (int k = 0; k < 3; ++k) t.trans[k] = g_drag.centre[k] + off[k];
    }
    if (!g_drag.first && value == g_drag.last) return;
    moveSelection(t, g_drag.first); // the first move of a drag leaves the undo step
    g_drag.first = false;
    g_drag.last = value;
    char b[64];
    snprintf(b, sizeof(b), g_drag.rotate ? "%+.1f deg" : "%+.2f m", value);
    dl->AddText(ImVec2(mouse.x + 16.f, mouse.y + 8.f), IM_COL32(255, 255, 255, 230), b);
}

void frame(bool menuOpen) {
    drawOutline();
    gizmoFrame(menuOpen);
    keysFrame(menuOpen);
    freeCamFrame(menuOpen);
    // Click to pick: a left click on the world (not on a window) while the Layout editor is open.
    const ImGuiIO& io = ImGui::GetIO();
    if (menuOpen && ImGui::GetFrameCount() - g_editorFrame <= 2 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse &&
        !g_gizmoHot) {
        float ray[6];
        if (cursorRay(ray)) pick(ray, io.KeyShift);
    }
}

} // namespace gn::host::layouts
