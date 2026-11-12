// The Field diff window (GridNode menu > Field diff): snapshot an object's bytes, do something in the
// game, and see which of its fields changed (reflected names, through its base classes; bytes no
// field covers by offset), compared once or live every frame. The diff itself is
// gridnode/field_diff.hpp (host-tested).

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/field_diff.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"
#include "imgui.h"

namespace gn::host::fielddiff {
namespace {

constexpr uint32_t kMaxBytes = 0x4000, kUnknownBytes = 0x400;

struct Snapshot {
    uintptr_t object = 0;
    const reflect::Type* type = nullptr;
    std::vector<uint8_t> bytes;
    std::vector<gn::fielddiff::FieldSpan> fields;
};
Snapshot g_snap;                              // (render thread)
uintptr_t g_entity = 0;                       // the entity whose components are listed (render thread)
const reflect::Type* g_entityType = nullptr;
std::vector<uint8_t> g_now;                   // the latest read
std::vector<gn::fielddiff::Change> g_changes; // since the snapshot
bool g_live = true, g_compared = false;
std::string g_status;

// An annotated member's size from its type's name (the annotations give none).
uint32_t memberSize(const char* type) {
    const std::string s = type ? type : "";
    if (s.find('*') != std::string::npos || s == "u64" || s == "i64" || s == "f64") return 8;
    if (s == "bool" || s == "u8" || s == "i8") return 1;
    if (s == "u16" || s == "i16") return 2;
    if (s == "Vec3") return 12;
    if (s == "Vec4" || s == "Quat") return 16;
    return 4;
}

// GridNode's annotated members (db/annotations) first, then the reflected fields, through the bases.
std::vector<gn::fielddiff::FieldSpan> fieldsOf(const reflect::Type* t) {
    std::vector<gn::fielddiff::FieldSpan> out;
    for (const reflect::Type* c = t; c; c = registry().super(c))
        for (const gn::engine::meta::Member& m : gn::engine::meta::classMembers)
            if (m.owner && c->name == m.owner) out.push_back({c == t ? std::string(m.name) : c->name + "." + m.name, m.offset, memberSize(m.type)});
    for (const reflect::Type* c = t; c; c = registry().super(c))
        for (const reflect::Field& f : c->fields) {
            const reflect::FieldInfo fi = registry().describe(c, f);
            out.push_back({c == t ? f.name : c->name + "." + f.name, f.offset, fi.size ? fi.size : 4});
        }
    return out;
}

bool readAll(uintptr_t p, size_t n, std::vector<uint8_t>& out) {
    out.assign(n, 0);
    // Page by page: an object near the end of its allocation may run into an unreadable page.
    size_t got = 0;
    while (got < n) {
        const size_t chunk = std::min<size_t>(n - got, 0x1000 - ((p + got) & 0xfff));
        if (!mem::read(p + got, out.data() + got, chunk)) break;
        got += chunk;
    }
    out.resize(got);
    return got > 0;
}

void take(uintptr_t p) {
    Snapshot s;
    s.object = p;
    s.type = registry().typeOf(mem::ProcessMemory(), p);
    const size_t n = s.type && s.type->size ? std::min<uint32_t>(s.type->size, kMaxBytes) : kUnknownBytes;
    if (!readAll(p, n, s.bytes)) {
        g_status = "Could not read that address";
        GN_LOG("field diff: %llx could not be read", static_cast<unsigned long long>(p));
        return;
    }
    if (s.type) s.fields = fieldsOf(s.type);
    char b[200];
    snprintf(b, sizeof(b), "Snapshot of %llx: %s, %llu bytes, %llu fields", static_cast<unsigned long long>(p),
             s.type ? s.type->name.c_str() : "unknown type", static_cast<unsigned long long>(s.bytes.size()),
             static_cast<unsigned long long>(s.fields.size()));
    g_status = b;
    GN_LOG("field diff: %s", b);
    // An entity: list its components. One of them: keep the list. Anything else: no list.
    static const reflect::Type* compEntity = registry().find("ComponentEntity");
    static const reflect::Type* component = registry().find("Component");
    const bool isComponent = s.type && component && registry().isKindOf(s.type, component);
    if (s.type && !isComponent && ((compEntity && registry().isKindOf(s.type, compEntity)) || !componentsOf(p).empty()))
        g_entity = p, g_entityType = s.type;
    else if (!isComponent) g_entity = 0, g_entityType = nullptr;
    g_snap = std::move(s);
    g_changes.clear();
    g_compared = false;
}

void compare() {
    if (g_snap.bytes.empty()) return;
    std::vector<uint8_t> now;
    if (!readAll(g_snap.object, g_snap.bytes.size(), now) || now.size() != g_snap.bytes.size()) {
        g_status = "The object can no longer be read (destroyed?)";
        GN_LOG("field diff: %llx can no longer be read", static_cast<unsigned long long>(g_snap.object));
        g_live = false;
        return;
    }
    g_now = std::move(now);
    g_changes = gn::fielddiff::diff(g_snap.bytes, g_now, g_snap.fields);
    g_compared = true;
}
// The character (server half: AI state lives there) nearest the middle of the screen, within 100 m;
// Faith left out.
// A server character's health component, found on the server thread (getComponent walks its
// components), handed to the render thread.
std::atomic<uintptr_t> g_healthFound{0};
std::atomic<bool> g_healthAsked{false};

void askHealth(uintptr_t character) {
    g_healthAsked = true;
    server().post([character] {
        uintptr_t hc = 0;
        static const reflect::Type* chars = registry().find("ServerCharacterEntity");
        const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), character);
        if (t && chars && registry().isKindOf(t, chars) && inspector::alive(character, t))
            for (const char* name : {"PamServerHumanHealthComponent", "PamServerHealthComponent", "ServerHealthComponent"})
                if (const reflect::Type* ht = registry().find(name)) {
                    hc = reinterpret_cast<uintptr_t>(getComponent(reinterpret_cast<void*>(character), reinterpret_cast<const gn_type*>(ht)));
                    if (hc) break;
                }
        g_healthFound = hc ? hc : 1; // 1: asked, none
    });
}

uintptr_t characterAhead() {
    gn_view v{};
    static const reflect::Type* chars = registry().find("ServerCharacterEntity");
    if (!chars || !layouts::view(v)) return 0;
    const uintptr_t faith = reinterpret_cast<uintptr_t>(localServerCharacter());
    uintptr_t best = 0;
    float bestScore = 0.15f; // off the view line by at most ~8.5 degrees
    for (const auto& [p, t] : inspector::trackedOf(chars)) {
        gn_transform at{};
        if (p == faith || !inspector::alive(p, t) || !entityTransform(reinterpret_cast<void*>(p), at)) continue;
        const float d[3] = {at.trans[0] - v.position[0], at.trans[1] + 1.2f - v.position[1], at.trans[2] - v.position[2]}; // her chest
        const float along = d[0] * v.forward[0] + d[1] * v.forward[1] + d[2] * v.forward[2];
        if (along < 0.5f || along > 100.f) continue;
        const float len2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        const float off = std::sqrt(std::max(0.f, len2 - along * along)) / along; // tangent of the angle off the view line
        if (off < bestScore) bestScore = off, best = p;
    }
    return best;
}
} // namespace

void draw(bool* open) {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 44, ImGui::GetFontSize() * 28), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Field diff", open)) {
        ImGui::End();
        return;
    }
    static char input[32] = {};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##obj", "object address (hex)", input, sizeof(input));
    ImGui::SameLine();
    if (ImGui::Button("Snapshot")) {
        const unsigned long long p = strtoull(input, nullptr, 16);
        if (p > 0x10000) take(static_cast<uintptr_t>(p));
        else g_status = "Type an object's address (the inspector shows them)";
    }
    ImGui::SameLine();
    if (ImGui::Button("Faith (client)"))
        if (void* f = localClientCharacter()) take(reinterpret_cast<uintptr_t>(f));
    ImGui::SameLine();
    if (ImGui::Button("Faith (server)"))
        if (void* f = localServerCharacter()) take(reinterpret_cast<uintptr_t>(f));
    ImGui::SameLine();
    if (ImGui::Button("Character ahead")) {
        if (const uintptr_t c = characterAhead()) take(c);
        else g_status = "No character near the middle of the screen (within 100 m)";
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The character nearest the middle of the screen (the server's copy)");
    if (g_healthAsked.load())
        if (const uintptr_t hc = g_healthFound.exchange(0)) {
            g_healthAsked = false;
            if (hc > 1) take(hc);
            else g_status = "That character has no health component GridNode knows";
        }
    if (!g_status.empty()) ImGui::TextDisabled("%s", g_status.c_str());
    if (g_entity) {
        // Its parts: pick one to diff instead (the entity itself holds little live state).
        const auto comps = componentsOf(g_entity);
        char label[160];
        snprintf(label, sizeof(label), "%s %llx: %d components###comps", g_entityType ? g_entityType->name.c_str() : "entity",
                 static_cast<unsigned long long>(g_entity), static_cast<int>(comps.size()));
        if (ImGui::CollapsingHeader(label)) {
            if (ImGui::Selectable("(the entity itself)", g_snap.object == g_entity)) take(g_entity);
            for (const auto& [c, t] : comps) {
                snprintf(label, sizeof(label), "%s  %llx", t->name.c_str(), static_cast<unsigned long long>(c));
                if (ImGui::Selectable(label, g_snap.object == c)) take(c);
            }
            static const reflect::Type* chars = registry().find("ServerCharacterEntity");
            if (comps.empty() && g_entityType && chars && registry().isKindOf(g_entityType, chars) && ImGui::Button("Its health component"))
                askHealth(g_entity); // the list came out empty: ask the game for the one it knows
        }
    }
    if (!g_snap.bytes.empty()) {
        ImGui::Checkbox("Live (every frame)", &g_live);
        ImGui::SameLine();
        if (ImGui::Button("Compare now") || g_live) compare();
        ImGui::SameLine();
        if (ImGui::Button("New snapshot")) take(g_snap.object);
        ImGui::SameLine();
        if (ImGui::Button("Log the changes") && g_compared) {
            GN_LOG("field diff: %llx (%s, %d reflected fields), %d changed since the snapshot:", static_cast<unsigned long long>(g_snap.object),
                   g_snap.type ? g_snap.type->name.c_str() : "unknown type", static_cast<int>(g_snap.fields.size()), static_cast<int>(g_changes.size()));
            for (const auto& c : g_changes) {
                const auto [was, now] = gn::fielddiff::formatBoth(g_snap.bytes.data() + c.offset, g_now.data() + c.offset, c.size);
                const bool named = c.name.empty() || c.name[0] != '+';
                GN_LOG("  +0x%x%s%s: %s -> %s", c.offset, named ? " " : "", named ? c.name.c_str() : "", was.c_str(), now.c_str());
            }
        }
        if (g_compared) ImGui::Text("%d changed", static_cast<int>(g_changes.size()));
        if (g_compared && ImGui::BeginTable("diff", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("offset");
            ImGui::TableSetupColumn("field");
            ImGui::TableSetupColumn("snapshot");
            ImGui::TableSetupColumn("now");
            ImGui::TableHeadersRow();
            for (const auto& c : g_changes) {
                const auto [was, now] = gn::fielddiff::formatBoth(g_snap.bytes.data() + c.offset, g_now.data() + c.offset, c.size);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("+0x%x", c.offset);
                ImGui::TableNextColumn();
                if (!c.name.empty() && c.name[0] == '+') ImGui::TextDisabled("(no field)");
                else ImGui::TextUnformatted(c.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(was.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(now.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

} // namespace gn::host::fielddiff
