// The Field diff window (GridNode menu > Field diff): snapshot an object's bytes, do something in the
// game, and see which of its fields changed (reflected names, through its base classes; bytes no
// field covers by offset), compared once or live every frame. The diff itself is
// gridnode/field_diff.hpp (host-tested).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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
std::vector<uint8_t> g_now;                   // the latest read
std::vector<gn::fielddiff::Change> g_changes; // since the snapshot
bool g_live = true, g_compared = false;
std::string g_status;

std::vector<gn::fielddiff::FieldSpan> fieldsOf(const reflect::Type* t) {
    std::vector<gn::fielddiff::FieldSpan> out;
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
        return;
    }
    if (s.type) s.fields = fieldsOf(s.type);
    char b[200];
    snprintf(b, sizeof(b), "Snapshot of %llx: %s, %llu bytes, %llu fields", static_cast<unsigned long long>(p),
             s.type ? s.type->name.c_str() : "unknown type", static_cast<unsigned long long>(s.bytes.size()),
             static_cast<unsigned long long>(s.fields.size()));
    g_status = b;
    g_snap = std::move(s);
    g_changes.clear();
    g_compared = false;
}

void compare() {
    if (g_snap.bytes.empty()) return;
    std::vector<uint8_t> now;
    if (!readAll(g_snap.object, g_snap.bytes.size(), now) || now.size() != g_snap.bytes.size()) {
        g_status = "The object can no longer be read (destroyed?)";
        g_live = false;
        return;
    }
    g_now = std::move(now);
    g_changes = gn::fielddiff::diff(g_snap.bytes, g_now, g_snap.fields);
    g_compared = true;
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
    if (!g_status.empty()) ImGui::TextDisabled("%s", g_status.c_str());
    if (!g_snap.bytes.empty()) {
        ImGui::Checkbox("Live (every frame)", &g_live);
        ImGui::SameLine();
        if (ImGui::Button("Compare now") || g_live) compare();
        ImGui::SameLine();
        if (ImGui::Button("New snapshot")) take(g_snap.object);
        ImGui::SameLine();
        if (ImGui::Button("Log the changes") && g_compared) {
            GN_LOG("field diff: %llx (%s), %d changed since the snapshot:", static_cast<unsigned long long>(g_snap.object),
                   g_snap.type ? g_snap.type->name.c_str() : "unknown type", static_cast<int>(g_changes.size()));
            for (const auto& c : g_changes)
                GN_LOG("  +0x%x %s: %s -> %s", c.offset, c.name.c_str(), gn::fielddiff::format(g_snap.bytes.data() + c.offset, c.size).c_str(),
                       gn::fielddiff::format(g_now.data() + c.offset, c.size).c_str());
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
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("+0x%x", c.offset);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(c.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(gn::fielddiff::format(g_snap.bytes.data() + c.offset, c.size).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(gn::fielddiff::format(g_now.data() + c.offset, c.size).c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

} // namespace gn::host::fielddiff
