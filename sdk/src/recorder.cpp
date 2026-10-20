// Recorder (M8): what the game does around an event, for research. Records every engine message
// (with its reflected fields, and the type of anything its unreflected slots point to), every
// entity creation (class, data, the blueprint of its bus, position), level changes and markers the
// user drops, each with the game's call stack at that moment. Pause, then export as JSON lines
// (recordings\<time>-<name>.jsonl next to gridnode.dll) to read, or to send for Ghidra work.
//
// Nothing in the game is changed: messages come from GridNode's own listeners (messages.cpp),
// creations from the creator slots GridNode already watches (entities.cpp). Stacks are taken with
// RtlCaptureStackBackTrace on the game's thread; frames in the game are written as image VAs
// (0x14...), which is what Ghidra shows.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/version.hpp"
#include "host.hpp"
#include "imgui.h"

namespace gn::host::recorder {
namespace {

namespace e = gn::engine;
using reflect::Type;

constexpr uint64_t kImageBase = 0x140000000ull;
constexpr size_t kMaxEvents = 250000;
constexpr int kStackFrames = 16;

enum Kind : uint8_t { kMessage, kCreated, kMarker, kLevel };

struct Ref {           // a pointer found in a message, with what it pointed to then
    uint16_t off;
    uintptr_t ptr;
    const Type* type;
};

struct Event {
    uint64_t seq = 0;
    double t = 0;                  // seconds since the recording started
    Kind kind = kMessage;
    uint32_t thread = 0;
    // message
    int manager = 0;
    uint32_t category = 0, typeId = 0, realm = 0;
    std::vector<uint8_t> bytes;
    std::vector<Ref> refs;
    // created
    uintptr_t ptr = 0;
    const Type* type = nullptr;
    uintptr_t data = 0;
    const Type* dataType = nullptr;
    std::string blueprint;         // the bus's blueprint (by name), if it has one
    float pos[3] = {};
    bool havePos = false;
    // marker / level
    std::string text;
    std::vector<uint64_t> stack;   // image VAs of game frames, innermost first
};

std::mutex g_m;
std::deque<Event> g_events;        // (g_m)
uint64_t g_seq = 0;                // (g_m)
uint64_t g_dropped = 0;            // (g_m) the oldest, beyond kMaxEvents
std::atomic<bool> g_recording{false};
std::atomic<bool> g_wantMessages{true}, g_wantCreated{true}, g_wantStacks{true};
LARGE_INTEGER g_freq{}, g_t0{};
std::wstring g_dir;
std::string g_lastExport;          // render thread
std::unordered_map<uint32_t, bool> g_ignored; // message type ids not recorded (g_m)
std::string g_level;               // (g_m)

double now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return g_freq.QuadPart ? static_cast<double>(c.QuadPart - g_t0.QuadPart) / static_cast<double>(g_freq.QuadPart) : 0.0;
}

// The game's frames of the current call stack (GridNode's own frames and system DLLs left out).
void captureStack(std::vector<uint64_t>& out) {
    if (!g_wantStacks.load(std::memory_order_relaxed)) return;
    void* frames[kStackFrames + 4];
    const USHORT n = RtlCaptureStackBackTrace(2, kStackFrames + 4, frames, nullptr);
    for (USHORT i = 0; i < n && out.size() < kStackFrames; ++i) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
        if (mem::inImage(a)) out.push_back(a - mem::moduleBase() + kImageBase);
    }
}

void push(Event&& ev) {
    std::lock_guard<std::mutex> lock(g_m);
    ev.seq = ++g_seq;
    g_events.push_back(std::move(ev));
    while (g_events.size() > kMaxEvents) {
        g_events.pop_front();
        ++g_dropped;
    }
}

// A CString field or an asset's Name: a char pointer.
std::string readCString(uintptr_t p) {
    if (!p) return {};
    char b[257] = {};
    for (size_t i = 0; i < 256; i += 32) {
        if (!mem::read(p + i, b + i, 32)) break;
        if (memchr(b + i, 0, 32)) break;
    }
    b[256] = 0;
    std::string s = b;
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) return {};
    return s;
}

// The Name of an asset (a Blueprint, ...), when its type has one.
std::string assetName(uintptr_t obj) {
    const Type* t = obj ? registry().typeOf(mem::ProcessMemory(), obj) : nullptr;
    reflect::FieldInfo fi;
    if (!t || !registry().findField(t, "Name", fi) || !fi.type || fi.type->name != "CString") return {};
    return readCString(mem::readOr<uintptr_t>(obj + fi.field->offset));
}

// The blueprint behind an entity bus: the bus's reference data (+0x38) points to it.
std::string busBlueprint(uintptr_t bus) {
    const uintptr_t ref = bus ? mem::readOr<uintptr_t>(bus + 0x38) : 0;
    const Type* rt = ref ? registry().typeOf(mem::ProcessMemory(), ref) : nullptr;
    if (!rt) return {};
    reflect::FieldInfo fi;
    if (registry().findField(rt, "Blueprint", fi)) {
        const std::string n = assetName(mem::readOr<uintptr_t>(ref + fi.field->offset));
        if (!n.empty()) return n;
    }
    return assetName(ref);
}

// ---- sources ----------------------------------------------------------------------------------
void onMessage(int manager, const uint8_t* msg, uint32_t category, uint32_t typeId, uint32_t realm) {
    if (!g_recording.load(std::memory_order_relaxed) || !g_wantMessages.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_ignored.count(typeId)) return;
    }
    Event ev;
    ev.t = now();
    ev.kind = kMessage;
    ev.thread = GetCurrentThreadId();
    ev.manager = manager, ev.category = category, ev.typeId = typeId, ev.realm = realm;
    const Type* t = messageTypeById(typeId);
    const size_t n = std::min<size_t>(t && t->size >= 0x30 ? t->size : 0x80, 0x400);
    ev.bytes.resize(n);
    if (!mem::read(reinterpret_cast<uintptr_t>(msg), ev.bytes.data(), n)) {
        ev.bytes.resize(0x30);
        memcpy(ev.bytes.data(), msg, 0x30);
    }
    // What its pointers point to now (the objects may be gone by the time anyone reads the export).
    for (size_t off = 0x30; off + 8 <= ev.bytes.size(); off += 8) {
        uint64_t q;
        memcpy(&q, &ev.bytes[off], 8);
        if (q < 0x10000 || (q & 7)) continue;
        if (const Type* pt = registry().typeOf(mem::ProcessMemory(), static_cast<uintptr_t>(q)))
            ev.refs.push_back({static_cast<uint16_t>(off), static_cast<uintptr_t>(q), pt});
    }
    captureStack(ev.stack);
    push(std::move(ev));
}

void onCreated(void* entity, void* ci, void*) {
    if (!g_recording.load(std::memory_order_relaxed) || !g_wantCreated.load(std::memory_order_relaxed) || !entity) return;
    Event ev;
    ev.t = now();
    ev.kind = kCreated;
    ev.thread = GetCurrentThreadId();
    ev.ptr = reinterpret_cast<uintptr_t>(entity);
    ev.type = registry().typeOf(mem::ProcessMemory(), ev.ptr);
    if (ci) {
        namespace c = e::st::EntityCreationInfo::off;
        const uintptr_t info = reinterpret_cast<uintptr_t>(ci);
        ev.havePos = mem::read(info + c::transform + 0x30, ev.pos, sizeof(ev.pos)) && std::isfinite(ev.pos[0]);
        ev.data = mem::readOr<uintptr_t>(info + c::data);
        ev.dataType = ev.data ? registry().typeOf(mem::ProcessMemory(), ev.data) : nullptr;
        ev.blueprint = busBlueprint(mem::readOr<uintptr_t>(info + c::bus));
    }
    captureStack(ev.stack);
    push(std::move(ev));
}

void marker(const std::string& name) {
    Event ev;
    ev.t = now();
    ev.kind = kMarker;
    ev.thread = GetCurrentThreadId();
    ev.text = name.empty() ? "marker" : name;
    gn_transform f{};
    if (void* faith = localClientCharacter(); faith && entityTransform(faith, f)) {
        memcpy(ev.pos, f.trans, sizeof(ev.pos));
        ev.havePos = true;
    }
    push(std::move(ev));
}

void setRecording(bool on) {
    if (on == g_recording.load()) return;
    if (on) {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_events.empty()) QueryPerformanceCounter(&g_t0); // time 0 is the start of a new recording
    }
    g_recording = on;
    if (on) messageTap(&onMessage);
    GN_LOG("recorder: %s", on ? "recording" : "paused");
}

void clearAll() {
    std::lock_guard<std::mutex> lock(g_m);
    g_events.clear();
    g_dropped = 0;
    QueryPerformanceCounter(&g_t0);
}

// ---- export (JSON lines) ----------------------------------------------------------------------
std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += static_cast<char>(c);
        else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
        } else o += static_cast<char>(c);
    }
    return o + "\"";
}

std::string hexAddr(uint64_t a) {
    char b[24];
    snprintf(b, sizeof(b), "\"0x%llx\"", static_cast<unsigned long long>(a));
    return b;
}

std::string num(double v) {
    if (!std::isfinite(v)) return "null";
    char b[32];
    snprintf(b, sizeof(b), "%.6g", v);
    return b;
}

const char* managerName(int m) { return m == 0 ? "core" : m == 1 ? "client" : "server"; }

// A reflected field of a copied message, as JSON (null when its type is not one we decode).
std::string fieldValue(const reflect::FieldInfo& fi, const std::vector<uint8_t>& b, size_t off) {
    if (!fi.type || off + std::max<uint32_t>(fi.size, 1) > b.size()) return "null";
    const std::string& n = fi.type->name;
    auto rd = [&](auto v) {
        memcpy(&v, &b[off], sizeof(v));
        return v;
    };
    if (fi.type->category == reflect::Category::Primitive || fi.type->category == reflect::Category::Enum) {
        if (n == "Boolean") return rd(uint8_t{}) ? "true" : "false";
        if (n == "Int8") return std::to_string(rd(int8_t{}));
        if (n == "Uint8") return std::to_string(rd(uint8_t{}));
        if (n == "Int16") return std::to_string(rd(int16_t{}));
        if (n == "Uint16") return std::to_string(rd(uint16_t{}));
        if (n == "Int32" || fi.type->category == reflect::Category::Enum) return std::to_string(rd(int32_t{}));
        if (n == "Uint32") return std::to_string(rd(uint32_t{}));
        if (n == "Int64") return std::to_string(rd(int64_t{}));
        if (n == "Uint64") return std::to_string(rd(uint64_t{}));
        if (n == "Float32") return num(rd(float{}));
        if (n == "Float64") return num(rd(double{}));
        if (n == "CString") return jsonStr(readCString(static_cast<uintptr_t>(rd(uint64_t{}))));
    }
    if (fi.type->category == reflect::Category::Class) return hexAddr(rd(uint64_t{}));
    if (n == "Vec2" || n == "Vec3" || n == "Vec4" || n == "LinearTransform" || n == "Quat") {
        const size_t count = n == "Vec2" ? 2 : n == "LinearTransform" ? 16 : n == "Vec3" ? 3 : 4;
        std::string o = "[";
        for (size_t i = 0; i < count && off + 4 * i + 4 <= b.size(); ++i) {
            float f;
            memcpy(&f, &b[off + 4 * i], 4);
            o += (i ? "," : "") + num(f);
        }
        return o + "]";
    }
    return "null";
}

std::string toJson(const Event& ev) {
    std::string o = "{\"seq\":" + std::to_string(ev.seq) + ",\"t\":" + num(ev.t) + ",\"thread\":" + std::to_string(ev.thread);
    switch (ev.kind) {
    case kMessage: {
        const Type* t = messageTypeById(ev.typeId);
        char id[16];
        snprintf(id, sizeof(id), "%08x", ev.typeId);
        o += ",\"kind\":\"message\",\"manager\":\"" + std::string(managerName(ev.manager)) + "\"";
        o += ",\"category\":" + jsonStr(messageCategoryName(ev.category));
        o += ",\"type\":" + jsonStr(t ? t->name : std::string(id));
        o += ",\"realm\":" + std::to_string(ev.realm);
        if (t && !t->fields.empty()) {
            o += ",\"fields\":{";
            bool first = true;
            for (const reflect::Field& f : t->fields) {
                const reflect::FieldInfo fi = registry().describe(t, f);
                o += (first ? "" : ",") + jsonStr(f.name) + ":" + fieldValue(fi, ev.bytes, f.offset);
                first = false;
            }
            o += "}";
        }
        if (!ev.refs.empty()) {
            o += ",\"pointers\":[";
            for (size_t i = 0; i < ev.refs.size(); ++i) {
                char b[48];
                snprintf(b, sizeof(b), "{\"off\":%u,\"ptr\":", ev.refs[i].off);
                o += (i ? "," : "") + std::string(b) + hexAddr(ev.refs[i].ptr) + ",\"type\":" + jsonStr(ev.refs[i].type->name) + "}";
            }
            o += "]";
        }
        std::string hex;
        hex.reserve(ev.bytes.size() * 2);
        for (uint8_t c : ev.bytes) {
            char b[3];
            snprintf(b, sizeof(b), "%02x", c);
            hex += b;
        }
        o += ",\"bytes\":\"" + hex + "\"";
        break;
    }
    case kCreated:
        o += ",\"kind\":\"created\",\"type\":" + jsonStr(ev.type ? ev.type->name : "?") + ",\"ptr\":" + hexAddr(ev.ptr);
        if (ev.dataType) o += ",\"data\":" + jsonStr(ev.dataType->name) + ",\"data_ptr\":" + hexAddr(ev.data);
        if (!ev.blueprint.empty()) o += ",\"blueprint\":" + jsonStr(ev.blueprint);
        break;
    case kMarker: o += ",\"kind\":\"marker\",\"name\":" + jsonStr(ev.text); break;
    case kLevel: o += ",\"kind\":\"level\",\"level\":" + jsonStr(ev.text); break;
    }
    if (ev.havePos) o += ",\"pos\":[" + num(ev.pos[0]) + "," + num(ev.pos[1]) + "," + num(ev.pos[2]) + "]";
    if (!ev.stack.empty()) {
        o += ",\"stack\":[";
        for (size_t i = 0; i < ev.stack.size(); ++i) o += (i ? "," : "") + hexAddr(ev.stack[i]);
        o += "]";
    }
    return o + "}\n";
}

bool validName(const std::string& n) {
    for (char c : n)
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return false;
    return n.size() <= 40;
}

// Writes everything recorded (a copy, on its own thread so the game does not stall).
void exportAll(const std::string& name) {
    std::vector<Event> events;
    uint64_t dropped;
    std::string level;
    {
        std::lock_guard<std::mutex> lock(g_m);
        events.assign(g_events.begin(), g_events.end());
        dropped = g_dropped;
        level = g_level;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::string file = std::string(stamp) + (name.empty() ? "" : "-" + name) + ".jsonl";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    const std::wstring path = g_dir + std::wstring(file.begin(), file.end());
    g_lastExport = "recordings\\" + file + " (writing...)";
    std::thread([events = std::move(events), dropped, level, path, file] {
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) {
            GN_LOG("recorder: cannot write recordings\\%s", file.c_str());
            return;
        }
        std::string head = "{\"kind\":\"header\",\"format\":\"gridnode-recording\",\"version\":1,\"gridnode\":\"" GRIDNODE_VERSION_STRING
                           "\",\"image_base\":\"0x140000000\",\"level\":" +
                           jsonStr(level) + ",\"events\":" + std::to_string(events.size()) + ",\"dropped_oldest\":" + std::to_string(dropped) +
                           "}\n";
        fwrite(head.data(), 1, head.size(), f);
        for (const Event& ev : events) {
            const std::string line = toJson(ev);
            fwrite(line.data(), 1, line.size(), f);
        }
        fclose(f);
        GN_LOG("recorder: exported %llu events to recordings\\%s", static_cast<unsigned long long>(events.size()), file.c_str());
    }).detach();
    g_lastExport = "recordings\\" + file;
}

// ---- keys -------------------------------------------------------------------------------------
constexpr int kKeyRecord = VK_F8, kKeyMarker = VK_HOME;
bool g_keyWas[2] = {};
char g_markerName[64] = "marker";
int g_markerCount = 0;

std::string nextMarkerName() {
    ++g_markerCount;
    char b[96];
    snprintf(b, sizeof(b), "%s %d", g_markerName[0] ? g_markerName : "marker", g_markerCount);
    return b;
}

} // namespace

void start(const std::wstring& gridnodeDir) {
    g_dir = gridnodeDir + L"recordings\\";
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    gn::host::onCreated("gridnode", "Entity", &onCreated, nullptr);
}

void onLevel(const std::string& level) {
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_level = level;
    }
    if (!g_recording.load()) return;
    Event ev;
    ev.t = now();
    ev.kind = kLevel;
    ev.text = level;
    push(std::move(ev));
}

void frame() {
    const bool focused = ui::gameFocused() && !ImGui::GetIO().WantTextInput;
    const int keys[2] = {kKeyRecord, kKeyMarker};
    bool pressed[2] = {};
    for (int i = 0; i < 2; ++i) {
        const bool down = focused && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        pressed[i] = down && !g_keyWas[i];
        g_keyWas[i] = down;
    }
    if (pressed[0]) setRecording(!g_recording.load());
    if (pressed[1] && g_recording.load()) marker(nextMarkerName());
    if (g_recording.load()) { // a small red dot, top right, while recording
        const ImGuiIO& io = ImGui::GetIO();
        const float r = ImGui::GetFontSize() * 0.4f;
        ImGui::GetForegroundDrawList()->AddCircleFilled(ImVec2(io.DisplaySize.x - r * 3.f, r * 3.f), r, IM_COL32(230, 40, 40, 220));
    }
}

void draw(bool* open) {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34, ImGui::GetFontSize() * 30), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Recorder", open)) {
        ImGui::End();
        return;
    }
    const bool rec = g_recording.load();
    size_t count;
    uint64_t dropped;
    double span = 0;
    {
        std::lock_guard<std::mutex> lock(g_m);
        count = g_events.size();
        dropped = g_dropped;
        if (!g_events.empty()) span = g_events.back().t - g_events.front().t;
    }
    if (ImGui::Button(rec ? "Pause (F8)" : count ? "Resume (F8)" : "Record (F8)")) setRecording(!rec);
    ImGui::SameLine();
    ImGui::BeginDisabled(rec || !count);
    if (ImGui::Button("Clear")) clearAll(), g_markerCount = 0;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (rec) ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f), "recording");
    else ImGui::TextDisabled(count ? "paused" : "not recording");
    ImGui::SameLine();
    ImGui::TextDisabled("%llu events over %.1f s%s", static_cast<unsigned long long>(count), span, dropped ? " (oldest dropped)" : "");

    bool m = g_wantMessages.load(), c = g_wantCreated.load(), s = g_wantStacks.load();
    if (ImGui::Checkbox("Messages", &m)) g_wantMessages = m;
    ImGui::SameLine();
    if (ImGui::Checkbox("Entities created", &c)) g_wantCreated = c;
    ImGui::SameLine();
    if (ImGui::Checkbox("Call stacks", &s)) g_wantStacks = s;

    ImGui::SeparatorText("Markers");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputText("##marker", g_markerName, sizeof(g_markerName));
    ImGui::SameLine();
    ImGui::BeginDisabled(!rec);
    if (ImGui::Button("Drop marker (Home)")) marker(nextMarkerName());
    ImGui::EndDisabled();
    ImGui::TextDisabled("Press Home in game right when the thing happens (got the KSEC, the door opened).");

    ImGui::SeparatorText("Export");
    static char name[48] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##name", "name (optional)", name, sizeof(name));
    ImGui::SameLine();
    const bool nameOk = validName(name);
    ImGui::BeginDisabled(!count || !nameOk);
    if (ImGui::Button("Export")) exportAll(name);
    ImGui::EndDisabled();
    if (!nameOk) ImGui::TextDisabled("Names: letters, digits, - and _");
    if (!g_lastExport.empty()) ImGui::TextDisabled("Last export: %s", g_lastExport.c_str());

    ImGui::SeparatorText("Ignore");
    ImGui::TextDisabled("Message types not recorded (frequent ones you do not need). Right-click a message below to ignore its type.");
    std::vector<uint32_t> ignored;
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (const auto& kv : g_ignored) ignored.push_back(kv.first);
    }
    for (uint32_t id : ignored) {
        const Type* t = messageTypeById(id);
        ImGui::PushID(static_cast<int>(id));
        if (ImGui::SmallButton((std::string(t ? t->name : "?") + "  x").c_str())) {
            std::lock_guard<std::mutex> lock(g_m);
            g_ignored.erase(id);
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    if (ignored.empty()) ImGui::TextDisabled("(none)");
    else ImGui::NewLine();

    ImGui::SeparatorText("Latest");
    if (ImGui::BeginChild("latest", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        std::vector<Event> tail;
        {
            std::lock_guard<std::mutex> lock(g_m);
            const size_t from = g_events.size() > 300 ? g_events.size() - 300 : 0;
            for (size_t i = from; i < g_events.size(); ++i) {
                const Event& src = g_events[i];
                Event lite; // without the bytes and the stack
                lite.seq = src.seq, lite.t = src.t, lite.kind = src.kind, lite.manager = src.manager, lite.typeId = src.typeId;
                lite.type = src.type, lite.blueprint = src.blueprint, lite.text = src.text;
                tail.push_back(std::move(lite));
            }
        }
        for (const Event& ev : tail) {
            char line[300];
            switch (ev.kind) {
            case kMessage: {
                const Type* t = messageTypeById(ev.typeId);
                snprintf(line, sizeof(line), "%9.3f  %-6s %s", ev.t, managerName(ev.manager), t ? t->name.c_str() : "?");
                ImGui::TextUnformatted(line);
                if (ImGui::BeginPopupContextItem(std::to_string(ev.seq).c_str())) {
                    if (ImGui::MenuItem("Do not record this type")) {
                        std::lock_guard<std::mutex> lock(g_m);
                        g_ignored[ev.typeId] = true;
                    }
                    ImGui::EndPopup();
                }
                break;
            }
            case kCreated:
                snprintf(line, sizeof(line), "%9.3f  new    %s%s%s", ev.t, ev.type ? ev.type->name.c_str() : "?", ev.blueprint.empty() ? "" : "  ",
                         ev.blueprint.c_str());
                ImGui::TextDisabled("%s", line);
                break;
            case kMarker:
                snprintf(line, sizeof(line), "%9.3f  ---- %s ----", ev.t, ev.text.c_str());
                ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "%s", line);
                break;
            case kLevel:
                snprintf(line, sizeof(line), "%9.3f  level %s", ev.t, ev.text.c_str());
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "%s", line);
                break;
            }
        }
        if (rec && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f) ImGui::SetScrollHereY(1.f);
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace gn::host::recorder
