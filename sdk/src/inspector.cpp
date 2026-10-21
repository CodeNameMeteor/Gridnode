// The in-game inspector: live entities by class, every reflected type, and an object view that
// decodes reflected fields (and GridNode's annotated members), follows pointers and edits values.
//
// Entities are tracked from the moment GridNode starts, through an on_created listener on Entity
// (every class the engine has a creator for). There is no destroy hook yet, so an entry counts as
// live while its vtable still says the class it was created as. GridNode's own pruning thread goes
// through the list all the time (whether or not any window is open), drops dead entries and
// remembers their addresses as dead until a new entity is created there; alive() answers from that,
// and every write through a tracked pointer asks it first. Everything is read fault tolerantly;
// edits are written on the client thread.

#include <windows.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"
#include "imgui.h"

namespace gn::host::inspector {
namespace {

namespace e = gn::engine;
using reflect::Category;
using reflect::Type;

// ---- tracked entities -------------------------------------------------------------------------
struct Tracked {
    uintptr_t ptr;
    const Type* type;
};
std::mutex g_m;
std::vector<Tracked> g_entities; // guarded by g_m; appended by game threads, pruned by the render thread
std::unordered_map<uintptr_t, size_t> g_index; // guarded by g_m: pointer -> its entry (memory is reused)
constexpr size_t kMaxTracked = 400000;
size_t g_pruneAt = 0;   // pruning thread
std::unordered_map<uintptr_t, uint8_t> g_dead; // guarded by g_m: addresses of entities found dead (until reused)
constexpr size_t kMaxDead = 200000;
bool g_started = false; // GridNode's thread, before the UI exists
size_t g_dropped = 0;   // guarded by g_m

const Type* typeOf(uintptr_t p) { return p ? registry().typeOf(mem::ProcessMemory(), p) : nullptr; }

void onEntity(void* entity, void*, void*) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(entity);
    const Type* t = typeOf(p);
    if (!t) return;
    std::lock_guard<std::mutex> lock(g_m);
    if (g_entities.size() >= kMaxTracked) {
        ++g_dropped;
        return;
    }
    g_dead.erase(p); // a new entity at that address
    const auto it = g_index.find(p);
    if (it != g_index.end()) { // the address was reused by a new entity
        g_entities[it->second].type = t;
        return;
    }
    g_index.emplace(p, g_entities.size());
    g_entities.push_back({p, t});
}

// A slice of the list: entries whose object no longer has its type are dropped, their addresses
// remembered as dead.
void prune() {
    constexpr size_t kPerFrame = 4000;
    std::vector<std::pair<size_t, Tracked>> slice;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_pruneAt >= g_entities.size()) g_pruneAt = 0;
        const size_t end = std::min(g_entities.size(), g_pruneAt + kPerFrame);
        for (size_t i = g_pruneAt; i < end; ++i) slice.push_back({i, g_entities[i]});
        g_pruneAt = end;
    }
    std::vector<std::pair<size_t, Tracked>> dead;
    for (const auto& s : slice)
        if (typeOf(s.second.ptr) != s.second.type) dead.push_back(s);
    if (dead.empty()) return;
    std::lock_guard<std::mutex> lock(g_m);
    // Descending, so a swap from the end never moves an entry we still have to remove. Appends only
    // add at the end, and only this thread removes, so the indices below are still ours.
    for (auto it = dead.rbegin(); it != dead.rend(); ++it) {
        const size_t i = it->first;
        // Still the same entry (not re-created at the same address since we looked)?
        if (i >= g_entities.size() || g_entities[i].ptr != it->second.ptr || g_entities[i].type != it->second.type) continue;
        g_index.erase(g_entities[i].ptr);
        if (g_dead.size() >= kMaxDead) g_dead.clear(); // bounded; the vtable test still applies
        g_dead[g_entities[i].ptr] = 1;
        if (i + 1 != g_entities.size()) {
            g_entities[i] = g_entities.back();
            g_index[g_entities[i].ptr] = i;
        }
        g_entities.pop_back();
    }
    if (g_pruneAt > g_entities.size()) g_pruneAt = g_entities.size();
}

// ---- helpers ---------------------------------------------------------------------------------
std::string display(const Type* t) {
    if (!t) return "?";
    if (!t->name.empty()) return t->name;
    return registry().data().displayName(t->ti);
}

bool ciContains(const std::string& hay, const char* needle) {
    if (!*needle) return true;
    const size_t n = strlen(needle);
    if (n > hay.size()) return false;
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        size_t k = 0;
        while (k < n && tolower(static_cast<unsigned char>(hay[i + k])) == tolower(static_cast<unsigned char>(needle[k]))) ++k;
        if (k == n) return true;
    }
    return false;
}

bool kindOf(const Type* t, const char* base) {
    const Type* b = registry().find(base);
    return t && b && registry().isKindOf(t, b);
}

// ---- object view -----------------------------------------------------------------------------
struct Target {
    uintptr_t ptr = 0;
    const Type* type = nullptr; // what it was opened as (its own type if known)
    std::string label;
};
std::vector<Target> g_history; // render thread
int g_at = -1;

void open(uintptr_t ptr, const Type* type, std::string label) {
    if (!ptr) return;
    if (const Type* own = typeOf(ptr)) type = own;
    if (g_at >= 0 && g_history[size_t(g_at)].ptr == ptr) return;
    g_history.resize(size_t(g_at + 1));
    g_history.push_back({ptr, type, std::move(label)});
    if (g_history.size() > 64) g_history.erase(g_history.begin());
    g_at = int(g_history.size()) - 1;
}

// The object on screen belongs to a settings class that touches online play and its changes have
// not been allowed: values are shown, not editable (render thread).
bool g_readOnly = false;
std::set<const Type*> g_allowed; // online settings classes the user allowed changes to

// The object an edit belongs to (the one on screen), checked again when the write happens.
struct Owner {
    uintptr_t ptr = 0;
    const Type* type = nullptr;
};
Owner g_editOwner; // render thread: set while the object view is drawn

bool ownerAlive(const Owner& o) { return !o.ptr || alive(o.ptr, o.type); }

template <class T> void postWrite(uintptr_t at, T v) {
    const Owner o = g_editOwner;
    client().post([at, v, o] {
        if (ownerAlive(o)) mem::write(at, v);
    });
}

void pointerLink(uintptr_t p, const Type* declared, const char* id) {
    if (!p) {
        ImGui::TextDisabled("null");
        return;
    }
    const Type* own = typeOf(p);
    char label[160];
    snprintf(label, sizeof(label), "0x%llx %s##%s", static_cast<unsigned long long>(p),
             own ? display(own).c_str() : (declared ? ("(" + display(declared) + "?)").c_str() : ""), id);
    if (ImGui::SmallButton(label)) open(p, own ? own : declared, own ? display(own) : display(declared));
}

void fieldsOf(uintptr_t base, const Type* t, int depth);

// One value of type `ft` at `at` (a field, or an array element).
void value(uintptr_t at, const Type* ft, const char* id, int depth) {
    if (!ft) {
        uint64_t raw = 0;
        mem::read(at, raw);
        ImGui::TextDisabled("0x%016llx", static_cast<unsigned long long>(raw));
        return;
    }
    ImGui::PushID(id);
    switch (ft->category) {
    case Category::Primitive: {
        ImGui::BeginDisabled(g_readOnly);
        const std::string& n = ft->name;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
        if (n == "Boolean") {
            bool v = mem::readOr<uint8_t>(at) != 0;
            if (ImGui::Checkbox("##v", &v)) postWrite<uint8_t>(at, v ? 1 : 0);
        } else if (n == "Float32") {
            float v = mem::readOr<float>(at);
            if (ImGui::InputFloat("##v", &v, 0, 0, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue)) postWrite(at, v);
        } else if (n == "Float64") {
            double v = mem::readOr<double>(at);
            if (ImGui::InputDouble("##v", &v, 0, 0, "%.6f", ImGuiInputTextFlags_EnterReturnsTrue)) postWrite(at, v);
        } else if (n == "Int8" || n == "Int16" || n == "Int32" || n == "Int64" || n == "Uint8" || n == "Uint16" || n == "Uint32" ||
                   n == "Uint64") {
            const bool sign = n[0] == 'I';
            // The width from the type itself (Int8 .. Uint64), not the reflected size, which may be off.
            const size_t size = n.find("64") != std::string::npos ? 8 : n.find("32") != std::string::npos ? 4 : n.find("16") != std::string::npos ? 2 : 1;
            int64_t v = 0;
            mem::read(at, &v, std::min<size_t>(size, 8));
            if (sign && size < 8) v = (v << (64 - size * 8)) >> (64 - size * 8);
            ImGuiDataType dt = sign ? ImGuiDataType_S64 : ImGuiDataType_U64;
            if (ImGui::InputScalar("##v", dt, &v, nullptr, nullptr, nullptr, ImGuiInputTextFlags_EnterReturnsTrue)) {
                const int64_t w = v;
                const Owner o = g_editOwner;
                client().post([at, w, size, o] {
                    if (ownerAlive(o)) mem::write(at, &w, std::min<size_t>(size, sizeof(w)));
                });
            }
        } else if (n == "CString") {
            std::string s;
            const uintptr_t p = mem::readOr<uintptr_t>(at);
            if (p && mem::readCString(p, s, 200)) ImGui::Text("\"%s\"", s.c_str());
            else ImGui::TextDisabled(p ? "(unreadable)" : "null");
        } else if (n == "Guid") {
            uint32_t g[4] = {};
            mem::read(at, g, sizeof(g));
            ImGui::Text("%08x-%08x-%08x-%08x", g[0], g[1], g[2], g[3]);
        } else {
            uint64_t raw = 0;
            mem::read(at, &raw, std::min<size_t>(ft->size ? ft->size : 8, 8));
            ImGui::TextDisabled("0x%llx", static_cast<unsigned long long>(raw));
        }
        ImGui::EndDisabled();
        break;
    }
    case Category::Enum: {
        ImGui::BeginDisabled(g_readOnly);
        int32_t v = mem::readOr<int32_t>(at);
        const char* name = nullptr;
        for (const auto& ev : ft->values)
            if (ev.value == v) name = ev.name.c_str();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        if (ImGui::BeginCombo("##v", name ? name : std::to_string(v).c_str())) {
            for (const auto& ev : ft->values)
                if (ImGui::Selectable(ev.name.c_str(), ev.value == v)) postWrite<int32_t>(at, ev.value);
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        break;
    }
    case Category::Class:
        pointerLink(mem::readOr<uintptr_t>(at), ft, "p");
        break;
    case Category::ValueType:
        if (depth > 6) {
            ImGui::TextDisabled("...");
        } else if (ImGui::TreeNode("##vt", "%s", display(ft).c_str())) {
            fieldsOf(at, ft, depth + 1);
            ImGui::TreePop();
        }
        break;
    case Category::Array: {
        // Frostbite arrays: a pointer to the elements, the count in the 4 bytes before them.
        const uintptr_t items = mem::readOr<uintptr_t>(at);
        const uint32_t count = items ? mem::readOr<uint32_t>(items - 4) : 0;
        const Type* el = registry().byTi(ft->elementTi);
        if (!items || !count) {
            ImGui::TextDisabled("%s, empty", display(ft).c_str());
        } else if (ImGui::TreeNode("##arr", "%s, %u", display(ft).c_str(), count)) {
            const uint32_t stride = el ? (el->category == Category::Class ? 8u : std::max<uint32_t>(el->size, 1)) : 8u;
            const uint32_t shown = std::min<uint32_t>(count, 200);
            for (uint32_t i = 0; i < shown; ++i) {
                char idx[16];
                snprintf(idx, sizeof(idx), "%u", i);
                ImGui::Text("[%u]", i);
                ImGui::SameLine();
                value(items + uintptr_t(i) * stride, el, idx, depth + 1);
            }
            if (count > shown) ImGui::TextDisabled("(%u more)", count - shown);
            ImGui::TreePop();
        }
        break;
    }
    default: {
        uint64_t raw = 0;
        mem::read(at, raw);
        ImGui::TextDisabled("%s 0x%llx", reflect::categoryName(ft->category), static_cast<unsigned long long>(raw));
    }
    }
    ImGui::PopID();
}

// The reflected fields of `t` and its bases (bases first), as rows of a table.
void fieldsOf(uintptr_t base, const Type* t, int depth) {
    std::vector<const Type*> chain;
    for (const Type* c = t; c && chain.size() < 32; c = registry().super(c)) chain.push_back(c);
    std::reverse(chain.begin(), chain.end());
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("fields", 4, flags)) return;
    ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.5f);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    if (depth == 0) ImGui::TableHeadersRow();
    for (const Type* c : chain) {
        // GridNode's own findings for this class or struct (db/annotations), then the reflected fields.
        for (const e::meta::Member& m : e::meta::classMembers) {
            if (!m.owner || c->name != m.owner) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.f, 1.f), "+%03x", m.offset);
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.f, 1.f), "%s", m.name);
            if (m.note && *m.note && ImGui::IsItemHovered()) ImGui::SetTooltip("GridNode annotation: %s", m.note);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", m.type);
            ImGui::TableNextColumn();
            const uintptr_t at = base + m.offset;
            const std::string ty = m.type;
            if (!ty.empty() && ty.back() == '*') {
                char id[48];
                snprintf(id, sizeof(id), "m%x", m.offset);
                std::string pointee = ty.substr(0, ty.size() - 1);
                pointerLink(mem::readOr<uintptr_t>(at), registry().find(pointee), id);
            } else if (ty == "bool" || ty == "u8") {
                ImGui::Text("%u", mem::readOr<uint8_t>(at));
            } else if (ty == "u32" || ty == "i32") {
                ImGui::Text("%u", mem::readOr<uint32_t>(at));
            } else if (ty == "f32") {
                ImGui::Text("%.4f", mem::readOr<float>(at));
            } else {
                ImGui::TextDisabled("0x%llx", static_cast<unsigned long long>(mem::readOr<uint64_t>(at)));
            }
        }
        if (c->fields.empty()) continue;
        if (chain.size() > 1) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", display(c).c_str());
        }
        for (const reflect::Field& f : c->fields) {
            const reflect::FieldInfo fi = registry().describe(c, f);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("+%03x", f.offset);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(f.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", display(fi.type).c_str());
            ImGui::TableNextColumn();
            char id[64];
            snprintf(id, sizeof(id), "%s%x", f.name.c_str(), f.offset);
            value(base + f.offset, fi.type, id, depth);
        }
    }
    ImGui::EndTable();
}

void hexView(uintptr_t p) {
    static int rows = 16;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    ImGui::SliderInt("rows", &rows, 4, 128);
    for (int r = 0; r < rows; ++r) {
        uint8_t b[16];
        const uintptr_t at = p + uintptr_t(r) * 16;
        if (!mem::read(at, b, 16)) {
            ImGui::TextDisabled("+%03x  (unreadable)", r * 16);
            break;
        }
        char line[128];
        int n = snprintf(line, sizeof(line), "+%03x ", r * 16);
        for (int i = 0; i < 16; ++i) n += snprintf(line + n, sizeof(line) - size_t(n), "%s%02x", i % 8 ? " " : "  ", b[i]);
        n += snprintf(line + n, sizeof(line) - size_t(n), "  ");
        for (int i = 0; i < 16 && n < int(sizeof(line)) - 1; ++i) line[n++] = (b[i] >= 32 && b[i] < 127) ? char(b[i]) : '.';
        line[n] = 0;
        ImGui::TextUnformatted(line);
        // A pointer to a known object in the row: a link.
        for (int q = 0; q < 2; ++q) {
            uintptr_t v = 0;
            memcpy(&v, b + q * 8, 8);
            if (v < 0x10000 || (v & 7)) continue;
            if (const Type* t = typeOf(v)) {
                ImGui::SameLine();
                char id[24];
                snprintf(id, sizeof(id), "h%d_%d", r, q);
                pointerLink(v, t, id);
            }
        }
    }
}

void objectView() {
    if (g_at < 0) {
        ImGui::TextDisabled("Pick an entity, a type's default instance, or enter an address.");
        return;
    }
    ImGui::BeginDisabled(g_at <= 0);
    if (ImGui::ArrowButton("back", ImGuiDir_Left)) --g_at;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(g_at + 1 >= int(g_history.size()));
    if (ImGui::ArrowButton("fwd", ImGuiDir_Right)) ++g_at;
    ImGui::EndDisabled();
    ImGui::SameLine();
    const Target& cur = g_history[size_t(g_at)];
    const Type* own = typeOf(cur.ptr);
    const Type* type = own ? own : cur.type;
    ImGui::Text("0x%llx  %s", static_cast<unsigned long long>(cur.ptr), display(type).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("copy")) {
        char b[32];
        snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(cur.ptr));
        ImGui::SetClipboardText(b);
    }
    if (!own && cur.type && kindOf(cur.type, "Entity")) {
        ImGui::TextColored(ImVec4(1.f, 0.55f, 0.4f, 1.f), "Its vtable no longer says %s: the entity is gone.", display(cur.type).c_str());
        return;
    }
    if (type) {
        std::string chain;
        for (const Type* s = registry().super(type); s; s = registry().super(s)) chain += " : " + display(s);
        if (!chain.empty()) ImGui::TextDisabled("%s", chain.c_str() + 3);
    }
    if (type && kindOf(type, "SpatialEntity")) {
        gn_transform x{};
        if (entityTransform(reinterpret_cast<void*>(cur.ptr), x))
            ImGui::Text("Position %.2f, %.2f, %.2f", x.trans[0], x.trans[1], x.trans[2]);
    }
    // Settings that touch online play: shown, editable only once allowed.
    g_readOnly = false;
    if (type) {
        if (const char* why = onlineWarning(type->name)) {
            const bool allowed = g_allowed.count(type) != 0;
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.65f, 0.3f, 1.f));
            ImGui::TextWrapped("Online: %s. Changing these can affect online play, EA services or your online profile "
                               "(Beat Revival included).", why);
            ImGui::PopStyleColor();
            bool on = allowed;
            if (ImGui::Checkbox("Allow changes to this class", &on)) {
                if (on) {
                    g_allowed.insert(type);
                    GN_LOG("inspector: changes allowed to %s (online: %s)", type->name.c_str(), why);
                } else {
                    g_allowed.erase(type);
                }
            }
            g_readOnly = !on;
        }
    }
    ImGui::Separator();
    if (ImGui::BeginChild("obj", ImVec2(0, 0))) {
        g_editOwner = {own ? cur.ptr : 0, own}; // writes check it is still this object when they happen
        if (type) fieldsOf(cur.ptr, type, 0);
        if (ImGui::CollapsingHeader("Memory")) hexView(cur.ptr);
    }
    ImGui::EndChild();
}

// ---- lists ------------------------------------------------------------------------------------
struct ClassCount {
    const Type* type;
    size_t count;
};
std::vector<ClassCount> g_counts; // render thread
DWORD g_countsAt = 0;
const Type* g_selClass = nullptr;

void refreshCounts() {
    if (GetTickCount() - g_countsAt < 500) return;
    g_countsAt = GetTickCount();
    std::map<const Type*, size_t> m;
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (const Tracked& t : g_entities) ++m[t.type];
    }
    g_counts.clear();
    for (const auto& kv : m) g_counts.push_back({kv.first, kv.second});
    std::sort(g_counts.begin(), g_counts.end(), [](const ClassCount& a, const ClassCount& b) { return a.type->name < b.type->name; });
}

void entitiesTab() {
    refreshCounts();
    static char filter[96] = "";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##ef", "filter classes", filter, sizeof(filter));
    size_t total = 0, dropped = 0;
    {
        std::lock_guard<std::mutex> lock(g_m);
        total = g_entities.size();
        dropped = g_dropped;
    }
    ImGui::TextDisabled("%llu live entities in %llu classes%s", static_cast<unsigned long long>(total),
                       static_cast<unsigned long long>(g_counts.size()), dropped ? " (list full)" : "");
    const float h = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginChild("classes", ImVec2(0, h * 0.5f), ImGuiChildFlags_Borders)) {
        for (const ClassCount& c : g_counts) {
            if (!ciContains(c.type->name, filter)) continue;
            char label[160];
            snprintf(label, sizeof(label), "%s  (%llu)", c.type->name.c_str(), static_cast<unsigned long long>(c.count));
            if (ImGui::Selectable(label, g_selClass == c.type)) g_selClass = c.type;
        }
    }
    ImGui::EndChild();
    if (ImGui::BeginChild("instances", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        if (g_selClass) {
            std::vector<uintptr_t> ptrs;
            {
                std::lock_guard<std::mutex> lock(g_m);
                for (const Tracked& t : g_entities)
                    if (t.type == g_selClass && ptrs.size() < 1000) ptrs.push_back(t.ptr);
            }
            ImGuiListClipper clip;
            clip.Begin(int(ptrs.size()));
            while (clip.Step()) {
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                    const uintptr_t p = ptrs[size_t(i)];
                    char label[96];
                    gn_transform x{};
                    if (kindOf(g_selClass, "SpatialEntity") && entityTransform(reinterpret_cast<void*>(p), x))
                        snprintf(label, sizeof(label), "0x%llx  (%.1f, %.1f, %.1f)", static_cast<unsigned long long>(p), x.trans[0],
                                 x.trans[1], x.trans[2]);
                    else
                        snprintf(label, sizeof(label), "0x%llx", static_cast<unsigned long long>(p));
                    if (ImGui::Selectable(label, g_at >= 0 && g_history[size_t(g_at)].ptr == p)) open(p, g_selClass, g_selClass->name);
                }
            }
        } else {
            ImGui::TextDisabled("Select a class.");
        }
    }
    ImGui::EndChild();
}

const Type* g_selType = nullptr;

void typesTab() {
    static char filter[96] = "";
    static int category = 0; // 0 all, then Class, ValueType, Enum
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    ImGui::InputTextWithHint("##tf", "filter types", filter, sizeof(filter));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##tc", &category, "All\0Classes\0Value types\0Enums\0");
    const auto& types = registry().data().types;
    std::vector<const Type*> shown;
    for (const Type& t : types) {
        if (t.name.empty() || !ciContains(t.name, filter)) continue;
        if (category == 1 && t.category != Category::Class) continue;
        if (category == 2 && t.category != Category::ValueType) continue;
        if (category == 3 && t.category != Category::Enum) continue;
        shown.push_back(&t);
    }
    const float h = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginChild("types", ImVec2(0, h * 0.45f), ImGuiChildFlags_Borders)) {
        ImGuiListClipper clip;
        clip.Begin(int(shown.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
                if (ImGui::Selectable(shown[size_t(i)]->name.c_str(), g_selType == shown[size_t(i)])) g_selType = shown[size_t(i)];
    }
    ImGui::EndChild();
    if (ImGui::BeginChild("type", ImVec2(0, 0), ImGuiChildFlags_Borders) && g_selType) {
        const Type* t = g_selType;
        ImGui::Text("%s  (%s, %s)", display(t).c_str(), reflect::categoryName(t->category), t->module.c_str());
        ImGui::TextDisabled("size 0x%x, align %u, TypeInfo 0x%llx", t->size, t->alignment,
                            static_cast<unsigned long long>(mem::va(static_cast<uintptr_t>(t->ti))));
        if (t->category == Category::Class) {
            std::string chain;
            for (const Type* s = registry().super(t); s; s = registry().super(s)) chain += " : " + display(s);
            if (!chain.empty()) ImGui::TextDisabled("%s", chain.c_str() + 3);
            if (!t->vtables.empty())
                ImGui::TextDisabled("vtable 0x%llx%s", static_cast<unsigned long long>(mem::va(static_cast<uintptr_t>(t->vtables[0]))),
                                    t->vtables.size() > 1 ? " (+ candidates)" : "");
            if (t->defaultInstance && ImGui::SmallButton("Open default instance"))
                open(static_cast<uintptr_t>(t->defaultInstance), t, t->name + " (default)");
        }
        if (t->category == Category::Enum) {
            for (const auto& v : t->values) ImGui::BulletText("%s = %d", v.name.c_str(), v.value);
        } else {
            for (const reflect::Field& f : t->fields) {
                const reflect::FieldInfo fi = registry().describe(t, f);
                ImGui::BulletText("+%03x  %s  %s", f.offset, f.name.c_str(), display(fi.type).c_str());
            }
        }
    }
    ImGui::EndChild();
}

void settingsTab() {
    static char filter[96] = "";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##sf", "filter settings", filter, sizeof(filter));
    const std::vector<SettingsEntry> list = settingsList();
    if (list.empty()) {
        ImGui::TextDisabled("No settings objects found (not registered yet, or the settings manager is unreadable).");
        return;
    }
    ImGui::TextDisabled("%llu settings objects; select one to see and edit all of its fields",
                        static_cast<unsigned long long>(list.size()));
    if (ImGui::BeginChild("settings", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        for (const SettingsEntry& e : list) {
            if (!ciContains(e.type->name, filter)) continue;
            const bool sel = g_at >= 0 && g_history[size_t(g_at)].ptr == e.object;
            if (e.online) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.65f, 0.3f, 1.f));
            if (ImGui::Selectable(e.type->name.c_str(), sel)) open(e.object, e.type, e.type->name);
            if (e.online) {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Online: %s", e.online);
            }
        }
    }
    ImGui::EndChild();
}

void shortcutsTab() {
    void* cc = localClientCharacter();
    void* sc = localServerCharacter();
    const uintptr_t sw = mem::readOr<uintptr_t>(mem::rva(e::global::ServerGameWorld));
    const uintptr_t cw = mem::readOr<uintptr_t>(mem::rva(e::global::ClientGameWorld));
    auto row = [](const char* what, uintptr_t p, const char* id) {
        ImGui::TextUnformatted(what);
        ImGui::SameLine(ImGui::GetFontSize() * 12);
        pointerLink(p, nullptr, id);
    };
    row("Faith (client)", reinterpret_cast<uintptr_t>(cc), "cc");
    row("Faith (server)", reinterpret_cast<uintptr_t>(sc), "sc");
    row("Server world", sw, "sw");
    row("Client world", cw, "cw");
    ImGui::Separator();
    static char addr[32] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    const bool go = ImGui::InputTextWithHint("##addr", "0x...", addr, sizeof(addr), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Inspect address") || go) {
        const uintptr_t p = static_cast<uintptr_t>(strtoull(addr, nullptr, 16));
        if (p) open(p, nullptr, addr);
    }
    ImGui::TextDisabled("Addresses are runtime addresses in the game process.");
}

// ---- messages -----------------------------------------------------------------------------------
struct Rec {
    uint64_t seq;
    DWORD tick;
    int manager;
    uint32_t category, typeId, realm;
    std::vector<uint8_t> bytes; // a copy of the message (it only lives during the call)
};
std::mutex g_recM;
std::vector<Rec> g_recs;                  // guarded by g_recM: the newest kMaxRecs, oldest first
std::map<uint32_t, uint64_t> g_recCounts; // guarded by g_recM: type id -> messages recorded
uint64_t g_recSeq = 0;                    // guarded by g_recM: messages recorded since Clear
DWORD g_recStart = 0;                     // guarded by g_recM: when recording started (for the time column)
constexpr size_t kMaxRecs = 5000;

void record(int manager, const uint8_t* msg, uint32_t category, uint32_t typeId, uint32_t realm) {
    const Type* t = messageTypeById(typeId);
    const size_t n = std::min<size_t>(t && t->size >= 0x30 ? t->size : 0x80, 0x400);
    Rec r{0, GetTickCount(), manager, category, typeId, realm, std::vector<uint8_t>(n)};
    if (!mem::read(reinterpret_cast<uintptr_t>(msg), r.bytes.data(), n)) {
        r.bytes.resize(0x30);
        memcpy(r.bytes.data(), msg, 0x30);
    }
    std::lock_guard<std::mutex> lock(g_recM);
    ++g_recCounts[typeId];
    r.seq = ++g_recSeq;
    g_recs.push_back(std::move(r));
    if (g_recs.size() > kMaxRecs) g_recs.erase(g_recs.begin(), g_recs.begin() + kMaxRecs / 4);
}

// A copied message, 8 bytes per row: the header named, then each slot as hex, two u32, two f32,
// and a link when it points at an object GridNode knows (it opens in the object view, right).
// Only the copy is shown: nothing past its end (that would be unrelated, changing memory).
void messageBytes(const std::vector<uint8_t>& b) {
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("bytes", 5, tf)) return;
    ImGui::TableSetupColumn("Offset");
    ImGui::TableSetupColumn("Bytes");
    ImGui::TableSetupColumn("as u32");
    ImGui::TableSetupColumn("as f32");
    ImGui::TableSetupColumn("Meaning", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (size_t off = 0; off + 8 <= b.size(); off += 8) {
        uint64_t q;
        uint32_t u[2];
        float f[2];
        memcpy(&q, &b[off], 8);
        memcpy(u, &b[off], 8);
        memcpy(f, &b[off], 8);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("+%02llx", static_cast<unsigned long long>(off));
        ImGui::TableNextColumn();
        char hex[32];
        snprintf(hex, sizeof(hex), "%02x %02x %02x %02x %02x %02x %02x %02x", b[off], b[off + 1], b[off + 2], b[off + 3], b[off + 4],
                 b[off + 5], b[off + 6], b[off + 7]);
        ImGui::TextUnformatted(hex);
        ImGui::TableNextColumn();
        ImGui::Text("%u  %u", u[0], u[1]);
        ImGui::TableNextColumn();
        auto fl = [](float v) { return std::isfinite(v) && std::fabs(v) < 1e7f && (v == 0.f || std::fabs(v) > 1e-6f); };
        if (fl(f[0]) || fl(f[1])) ImGui::Text("%.3g  %.3g", f[0], f[1]);
        else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        if (off == 0) ImGui::TextDisabled("header: the message's vtable");
        else if (off == 8) ImGui::TextDisabled("header: category id, type id");
        else if (off == 0x10) ImGui::TextDisabled("header: realm");
        else if (off < 0x30) ImGui::TextDisabled("header");
        else if (q >= 0x10000 && !(q & 7)) {
            if (const Type* t = typeOf(static_cast<uintptr_t>(q))) {
                char id[16];
                snprintf(id, sizeof(id), "mb%llx", static_cast<unsigned long long>(off));
                pointerLink(static_cast<uintptr_t>(q), t, id);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens in the object view (right)");
            }
        }
    }
    ImGui::EndTable();
}

const char* managerName(int m) { return m == 0 ? "core" : m == 1 ? "client" : "server"; }

std::string messageName(uint32_t typeId) {
    if (const Type* t = messageTypeById(typeId)) return t->name;
    char b[16];
    snprintf(b, sizeof(b), "%08x", typeId);
    return b;
}

// Inspector → Messages. One switch (Record), a list of what was recorded (newest at the bottom; it
// only follows new messages while scrolled to the end, so rows do not move under the mouse), and
// a list of every message type with how often it was seen: clicking a type shows only its messages.
void messagesTab() {
    if (!messagesAvailable()) {
        ImGui::TextDisabled("The message manager functions are not as expected in this build.");
        return;
    }
    static bool recording = false;
    static uint32_t typeFilter = 0; // 0: all types
    static uint64_t sel = 0;
    static Rec selRec{};
    static bool showTypes = false;  // switch to the Messages view after picking a type

    if (ImGui::Button(recording ? "Stop recording" : "Start recording")) {
        recording = !recording;
        if (recording) {
            std::lock_guard<std::mutex> lock(g_recM);
            if (!g_recStart) g_recStart = GetTickCount();
        }
        messageMonitor(recording, &record);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        std::lock_guard<std::mutex> lock(g_recM);
        g_recs.clear();
        g_recCounts.clear();
        g_recSeq = 0;
        g_recStart = GetTickCount();
        sel = 0;
    }
    uint64_t total = 0;
    size_t kept = 0;
    {
        std::lock_guard<std::mutex> lock(g_recM);
        total = g_recSeq;
        kept = g_recs.size();
    }
    ImGui::SameLine();
    if (recording) ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "recording");
    else ImGui::TextDisabled("stopped");
    ImGui::SameLine();
    ImGui::TextDisabled("%llu messages (%llu kept)", static_cast<unsigned long long>(total), static_cast<unsigned long long>(kept));

    static char filter[96] = "";
    ImGui::SetNextItemWidth(typeFilter ? -ImGui::GetFontSize() * 14 : -1);
    ImGui::InputTextWithHint("##mf", "filter by type or category name", filter, sizeof(filter));
    if (typeFilter) {
        ImGui::SameLine();
        char b[200];
        snprintf(b, sizeof(b), "only %s  x", messageName(typeFilter).c_str());
        if (ImGui::SmallButton(b)) typeFilter = 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Showing one type only: click to show all");
    }

    if (ImGui::BeginTabBar("msgviews")) {
        const ImGuiTabItemFlags msgFlags = showTypes ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("Messages", nullptr, msgFlags)) {
            showTypes = false;
            const float detailH = sel ? ImGui::GetContentRegionAvail().y * 0.45f : 0.f;
            if (ImGui::BeginChild("list", ImVec2(0, -detailH), ImGuiChildFlags_Borders)) {
                std::lock_guard<std::mutex> lock(g_recM);
                std::vector<const Rec*> shown;
                for (const Rec& r : g_recs) {
                    if (typeFilter && r.typeId != typeFilter) continue;
                    if (*filter && !ciContains(messageName(r.typeId), filter) && !ciContains(messageCategoryName(r.category), filter)) continue;
                    shown.push_back(&r);
                }
                if (shown.empty())
                    ImGui::TextDisabled(recording ? "Nothing yet." : "Click Start recording to see every message the engine sends.");
                const bool atEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f;
                ImGuiListClipper clip;
                clip.Begin(int(shown.size()));
                while (clip.Step())
                    for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                        const Rec& r = *shown[size_t(i)];
                        const std::string cat = messageCategoryName(r.category);
                        char label[220];
                        snprintf(label, sizeof(label), "%8.3f  %-6s %-18s %s##%llu", (r.tick - g_recStart) / 1000.0, managerName(r.manager),
                                 cat.c_str(), messageName(r.typeId).c_str(), static_cast<unsigned long long>(r.seq));
                        const bool online = onlineWarning(cat) != nullptr;
                        if (online) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.65f, 0.3f, 1.f));
                        if (ImGui::Selectable(label, sel == r.seq)) {
                            sel = r.seq;
                            selRec = r; // a copy: the list may drop it while it is shown
                        }
                        if (online) ImGui::PopStyleColor();
                    }
                if (recording && atEnd) ImGui::SetScrollHereY(1.f); // follow new messages only when at the end
            }
            ImGui::EndChild();
            if (sel) {
                if (ImGui::BeginChild("msg", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
                    ImGui::Text("%s", messageName(selRec.typeId).c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("close")) sel = 0;
                    ImGui::TextDisabled("category %s, %s message manager, realm 0x%x, %.3f s; %llu bytes copied when it was sent",
                                        messageCategoryName(selRec.category).c_str(), managerName(selRec.manager), selRec.realm,
                                        (selRec.tick - g_recStart) / 1000.0, static_cast<unsigned long long>(selRec.bytes.size()));
                    ImGui::TextDisabled("This is a copy taken when the message was sent: it does not change. Members after the header are not "
                                        "reflected; a link opens the object it points to (still live) in the object view.");
                    messageBytes(selRec.bytes);
                }
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Types")) {
            static bool onlySeen = false;
            ImGui::Checkbox("Only types recorded so far", &onlySeen);
            ImGui::SameLine();
            ImGui::TextDisabled("(click a type to see its messages)");
            std::map<uint32_t, uint64_t> counts;
            {
                std::lock_guard<std::mutex> lock(g_recM);
                counts = g_recCounts;
            }
            struct Row {
                const MessageType* m;
                uint64_t seen;
            };
            std::vector<Row> rows;
            for (const MessageType& m : messageTypes()) {
                if (*filter && !ciContains(m.type->name, filter) && !ciContains(m.categoryName, filter)) continue;
                auto it = counts.find(m.typeId);
                const uint64_t seen = it != counts.end() ? it->second : 0;
                if (onlySeen && !seen) continue;
                rows.push_back({&m, seen});
            }
            std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.seen > b.seen; });
            const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
            if (ImGui::BeginTable("types", 4, tf)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Seen", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 2.f);
                ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch, 1.f);
                ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3);
                ImGui::TableHeadersRow();
                ImGuiListClipper clip;
                clip.Begin(int(rows.size()));
                while (clip.Step())
                    for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                        const Row& r = rows[size_t(i)];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (r.seen) ImGui::Text("%llu", static_cast<unsigned long long>(r.seen));
                        else ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        if (r.m->online) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.65f, 0.3f, 1.f));
                        if (ImGui::Selectable(r.m->type->name.c_str(), typeFilter == r.m->typeId, ImGuiSelectableFlags_SpanAllColumns)) {
                            typeFilter = r.m->typeId;
                            showTypes = true; // jump to its messages
                        }
                        if (r.m->online) ImGui::PopStyleColor();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", r.m->categoryName);
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%u", r.m->type->size);
                    }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ---- console ----------------------------------------------------------------------------------
std::mutex g_conM;
std::vector<std::string> g_conLines; // guarded by g_conM: the scrollback
bool g_conScroll = false;            // guarded by g_conM

void conPrint(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_conM);
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        const std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() || nl != std::string::npos) g_conLines.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (g_conLines.size() > 2000) g_conLines.erase(g_conLines.begin(), g_conLines.begin() + 500);
    g_conScroll = true;
}

// Command history (Up / Down in the input line, as in a command prompt) and Tab completion.
std::vector<std::string> g_cmdHistory; // render thread; oldest first
int g_cmdHistPos = -1;                 // -1: a new line; otherwise an index into g_cmdHistory

void remember(const std::string& line) {
    g_cmdHistPos = -1;
    if (!g_cmdHistory.empty() && g_cmdHistory.back() == line) return;
    g_cmdHistory.push_back(line);
    if (g_cmdHistory.size() > 200) g_cmdHistory.erase(g_cmdHistory.begin());
}

int inputCallback(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        if (g_cmdHistory.empty()) return 0;
        const int before = g_cmdHistPos;
        if (d->EventKey == ImGuiKey_UpArrow)
            g_cmdHistPos = g_cmdHistPos == -1 ? int(g_cmdHistory.size()) - 1 : std::max(0, g_cmdHistPos - 1);
        else if (d->EventKey == ImGuiKey_DownArrow && g_cmdHistPos != -1)
            g_cmdHistPos = g_cmdHistPos + 1 >= int(g_cmdHistory.size()) ? -1 : g_cmdHistPos + 1;
        if (before != g_cmdHistPos) {
            d->DeleteChars(0, d->BufTextLen);
            if (g_cmdHistPos >= 0) d->InsertChars(0, g_cmdHistory[size_t(g_cmdHistPos)].c_str());
        }
    } else if (d->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        // Complete the first word from the command names; list the candidates when it is ambiguous.
        const std::string text(d->Buf, size_t(d->BufTextLen));
        if (text.find(' ') != std::string::npos) return 0;
        const auto* cmds = static_cast<const std::vector<ConsoleCommand>*>(d->UserData);
        std::vector<const char*> hits;
        for (const ConsoleCommand& c : *cmds)
            if (_strnicmp(c.full, text.c_str(), text.size()) == 0) hits.push_back(c.full);
        if (hits.empty()) return 0;
        std::string common = hits[0];
        for (const char* h : hits) {
            size_t n = 0;
            while (n < common.size() && h[n] && tolower(static_cast<unsigned char>(h[n])) == tolower(static_cast<unsigned char>(common[n]))) ++n;
            common.resize(n);
        }
        if (hits.size() > 1 && common.size() <= text.size()) {
            std::string list = "  ";
            for (size_t i = 0; i < hits.size() && i < 40; ++i) list += std::string(hits[i]) + "  ";
            if (hits.size() > 40) list += "...";
            conPrint(list);
        }
        if (common.size() >= text.size()) {
            d->DeleteChars(0, d->BufTextLen);
            d->InsertChars(0, common.c_str());
            if (hits.size() == 1) d->InsertChars(d->CursorPos, " ");
        }
    }
    return 0;
}

void conRun(const std::string& line) {
    remember(line);
    conPrint("> " + line);
    client().post([line] { conPrint(consoleRun(line)); });
}

} // namespace

void drawConsole(bool* open) {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 44, ImGui::GetFontSize() * 26), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Console###gridnode_console", open)) {
        ImGui::End();
        return;
    }
    if (!consoleAvailable()) {
        ImGui::TextDisabled("The console functions are not as expected in this build; the console is off.");
        ImGui::End();
        return;
    }
    static char input[512] = "";
    static std::string pending;   // an online command waiting for confirmation
    static const char* pendingWhy = nullptr;
    static bool askConfirm = false; // open the confirmation (from anywhere in the window)
    static char filter[96] = "";
    const std::vector<ConsoleCommand> cmds = consoleCommands();

    if (ImGui::BeginTable("con", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("out", ImGuiTableColumnFlags_WidthStretch, 0.62f);
        ImGui::TableSetupColumn("cmds", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const float inputH = ImGui::GetFrameHeightWithSpacing() * 1.2f;
        if (ImGui::BeginChild("scroll", ImVec2(0, -inputH), ImGuiChildFlags_Borders)) {
            std::lock_guard<std::mutex> lock(g_conM);
            ImGuiListClipper clip;
            clip.Begin(int(g_conLines.size()));
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                    const std::string& l = g_conLines[size_t(i)];
                    if (!l.empty() && l[0] == '>') ImGui::TextColored(ImVec4(1.f, 0.6f, 0.45f, 1.f), "%s", l.c_str());
                    else ImGui::TextUnformatted(l.c_str());
                }
            if (g_conScroll) {
                ImGui::SetScrollHereY(1.f);
                g_conScroll = false;
            }
        }
        ImGui::EndChild();
        static bool refocus = false;
        if (refocus) {
            ImGui::SetKeyboardFocusHere(); // keep typing after Enter
            refocus = false;
        }
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4);
        bool run = ImGui::InputTextWithHint("##in", "command (Up/Down: history, Tab: complete)", input, sizeof(input),
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
                                                ImGuiInputTextFlags_CallbackCompletion,
                                            &inputCallback, const_cast<std::vector<ConsoleCommand>*>(&cmds));
        ImGui::SameLine();
        run |= ImGui::Button("Run");
        if (run && input[0]) {
            const std::string line = input;
            if (const char* why = commandOnline(line)) {
                pending = line;
                pendingWhy = why;
                askConfirm = true;
            } else {
                conRun(line);
            }
            input[0] = 0;
            refocus = true;
        }
        if (askConfirm) {
            ImGui::OpenPopup("Online command");
            askConfirm = false;
        }
        if (ImGui::BeginPopupModal("Online command", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", pending.c_str());
            ImGui::TextColored(ImVec4(1.f, 0.65f, 0.3f, 1.f), "Online: %s.", pendingWhy ? pendingWhy : "");
            ImGui::TextWrapped("This can affect online play, EA services or your online profile (Beat Revival included).");
            if (ImGui::Button("Run it")) {
                GN_LOG("console: online command run from the menu: %s", pending.c_str());
                conRun(pending);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                remember(pending); // still in the history, to fix and run again
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##cf", "filter commands", filter, sizeof(filter));
        ImGui::TextDisabled("%llu commands. Click to fill in, double-click to run.", static_cast<unsigned long long>(cmds.size()));
        if (ImGui::BeginChild("cmds", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            std::vector<const ConsoleCommand*> shown;
            for (const ConsoleCommand& c : cmds)
                if (ciContains(c.full, filter)) shown.push_back(&c);
            ImGuiListClipper clip;
            clip.Begin(int(shown.size()));
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                    const ConsoleCommand& c = *shown[size_t(i)];
                    ImGui::PushID(i);
                    if (c.online) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.65f, 0.3f, 1.f));
                    else if (c.setting) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.f, 1.f));
                    if (ImGui::Selectable(c.full, false, ImGuiSelectableFlags_AllowDoubleClick)) {
                        snprintf(input, sizeof(input), "%s%s", c.full, c.setting ? " " : "");
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            if (c.online) {
                                pending = c.full;
                                pendingWhy = c.online;
                                askConfirm = true;
                            } else {
                                conRun(c.full);
                            }
                        }
                    }
                    if (c.online || c.setting) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered() && (*c.help || c.online))
                        ImGui::SetTooltip("%s%s%s%s", c.setting ? "Setting. " : "", c.help, c.online ? "\nOnline: " : "",
                                          c.online ? c.online : "");
                    ImGui::PopID();
                }
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::End();
}

std::vector<std::pair<uintptr_t, const reflect::Type*>> tracked() {
    std::lock_guard<std::mutex> lock(g_m);
    std::vector<std::pair<uintptr_t, const reflect::Type*>> out;
    out.reserve(g_entities.size());
    for (const Tracked& t : g_entities) out.push_back({t.ptr, t.type});
    return out;
}

bool alive(uintptr_t p, const reflect::Type* type) {
    if (!p || !type) return false;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_dead.count(p)) return false;
    }
    return typeOf(p) == type;
}

void markDead(uintptr_t p) {
    if (!p) return;
    std::lock_guard<std::mutex> lock(g_m);
    if (g_dead.size() >= kMaxDead) g_dead.clear();
    g_dead[p] = 1; // its tracked entry is dropped by the pruning thread once the memory changes
}

void start() {
    if (g_started) return;
    g_started = true;
    // Pruning, on GridNode's own thread (reads only), so dead entries go whether or not a window is open.
    std::thread([] {
        for (;;) {
            Sleep(15);
            prune();
        }
    }).detach();
    g_entities.reserve(65536);
    g_index.reserve(65536);
    onCreated("gridnode", "Entity", &onEntity, nullptr);
    hookDestructors(); // dead the moment they are destroyed (pruning stays as the backstop)
}

void draw(bool* open) {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetFontSize() * 24, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 52, ImGui::GetFontSize() * 34), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector###gridnode_inspector", open)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTable("split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthStretch, 0.62f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::BeginChild("left", ImVec2(0, 0))) {
            if (ImGui::BeginTabBar("tabs")) {
                if (ImGui::BeginTabItem("Entities")) {
                    entitiesTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Types")) {
                    typesTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Settings")) {
                    settingsTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Messages")) {
                    messagesTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Shortcuts")) {
                    shortcutsTab();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        if (ImGui::BeginChild("right", ImVec2(0, 0))) objectView();
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace gn::host::inspector
