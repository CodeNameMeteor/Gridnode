// The function tracer (GridNode menu, Tracer): pick a game function by address or annotated name, and
// every call to it is kept (time, thread, rcx / rdx / r8 / r9 and the next two stack arguments, xmm0..3,
// the caller), shown with the arguments' types and the caller's function, and written to the log on
// request. Built on the function detours (patches.cpp): a small stub per traced function saves the
// argument registers, hands them to record(), puts them back and jumps to the function. Return values
// are not traced.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"
#include "imgui.h"

namespace gn::host::tracer {
namespace {

namespace e = gn::engine;

constexpr size_t kKept = 2048; // calls kept per traced function (the newest)

struct Call {
    uint64_t when = 0; // QueryPerformanceCounter
    uint32_t thread = 0;
    uint64_t args[6] = {}; // rcx, rdx, r8, r9, then the 5th and 6th (stack)
    float floats[4] = {};  // xmm0..3, low float
    uint64_t caller = 0;   // return address
};

struct Trace {
    uint64_t va = 0;           // the function (image va)
    std::string name;
    uint8_t* stub = nullptr;   // executable, never freed (a thread may still be in it)
    uint64_t* original = nullptr; // in the stub's page: what the stub jumps to (the hook table rewrites it)
    std::atomic<uint64_t> calls{0};
    std::atomic<bool> paused{false};
    std::unique_ptr<Call[]> ring{new Call[kKept]};
};

std::mutex g_m;
std::vector<std::unique_ptr<Trace>> g_traces; // (g_m) never shrinks while running: stubs point at them
std::atomic<bool> g_on{true};                 // false: stubs go straight on (unloading)

// Called from a stub with the saved registers: regs[0] rax, [1] r9, [2] r8, [3] rdx, [4] rcx, [5] the
// return address, [6..9] the caller's home space, [10] / [11] the 5th and 6th arguments; xmm0..3 saved
// 0x40 bytes below regs.
extern "C" void gnTraceRecord(Trace* t, const uint64_t* regs) {
    if (!g_on.load(std::memory_order_relaxed) || t->paused.load(std::memory_order_relaxed)) return;
    const uint64_t n = t->calls.fetch_add(1, std::memory_order_relaxed);
    Call& c = t->ring[n % kKept];
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    c.when = static_cast<uint64_t>(q.QuadPart);
    c.thread = GetCurrentThreadId();
    c.args[0] = regs[4], c.args[1] = regs[3], c.args[2] = regs[2], c.args[3] = regs[1];
    c.args[4] = regs[10], c.args[5] = regs[11];
    const uint8_t* x = reinterpret_cast<const uint8_t*>(regs) - 0x40;
    for (int i = 0; i < 4; ++i) memcpy(&c.floats[i], x + i * 16, 4);
    c.caller = regs[5];
}

// The stub: push rcx, rdx, r8, r9, rax; sub rsp 0x60 (16-aligned again: the function was entered at
// 8 mod 16); movups [rsp+0x20..0x50], xmm0..3; rcx = trace, rdx = rsp + 0x60; call gnTraceRecord;
// the registers back; jmp [cell], the cell (at +0x200 in the stub's page) holding the original, which
// the hook table keeps up to date.
constexpr size_t kCell = 0x200;
uint8_t* makeStub(Trace* t) {
    std::vector<uint8_t> c;
    auto put = [&](std::initializer_list<uint8_t> b) { c.insert(c.end(), b); };
    auto put64 = [&](uint64_t v) { for (int i = 0; i < 8; ++i) c.push_back(uint8_t(v >> (i * 8))); };
    put({0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x50});          // push rcx, rdx, r8, r9, rax
    put({0x48, 0x83, 0xEC, 0x60});                            // sub rsp, 0x60
    put({0x0F, 0x11, 0x44, 0x24, 0x20});                      // movups [rsp+0x20], xmm0
    put({0x0F, 0x11, 0x4C, 0x24, 0x30});                      // movups [rsp+0x30], xmm1
    put({0x0F, 0x11, 0x54, 0x24, 0x40});                      // movups [rsp+0x40], xmm2
    put({0x0F, 0x11, 0x5C, 0x24, 0x50});                      // movups [rsp+0x50], xmm3
    put({0x48, 0xB9}), put64(reinterpret_cast<uint64_t>(t)); // mov rcx, t
    put({0x48, 0x8D, 0x54, 0x24, 0x60});                      // lea rdx, [rsp+0x60]
    put({0x48, 0xB8}), put64(reinterpret_cast<uint64_t>(&gnTraceRecord)); // mov rax, gnTraceRecord
    put({0xFF, 0xD0});                                        // call rax
    put({0x0F, 0x10, 0x44, 0x24, 0x20});                      // movups xmm0, [rsp+0x20]
    put({0x0F, 0x10, 0x4C, 0x24, 0x30});
    put({0x0F, 0x10, 0x54, 0x24, 0x40});
    put({0x0F, 0x10, 0x5C, 0x24, 0x50});
    put({0x48, 0x83, 0xC4, 0x60});                            // add rsp, 0x60
    put({0x58, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59});          // pop rax, r9, r8, rdx, rcx
    const int32_t disp = static_cast<int32_t>(kCell - (c.size() + 6));
    put({0xFF, 0x25});                                        // jmp [rip+disp]: the cell
    for (int i = 0; i < 4; ++i) c.push_back(uint8_t(uint32_t(disp) >> (i * 8)));
    void* mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem || c.size() > kCell) return nullptr;
    memcpy(mem, c.data(), c.size());
    t->original = reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(mem) + kCell);
    return static_cast<uint8_t*>(mem);
}

uint64_t lookup(const std::string& text) {
    if (text.empty()) return 0;
    const char* s = text.c_str();
    if (text.rfind("FUN_", 0) == 0) s += 4;
    char* end = nullptr;
    const unsigned long long v = strtoull(s, &end, 16);
    if (end && *end == 0 && v >= 0x140000000ull) return v;
    for (const auto& f : engine::meta::functionsByAddress)
        if (text == f.name) return f.va;
    return 0;
}

std::string nameOf(uint64_t va) {
    const auto& t = engine::meta::functionsByAddress;
    auto it = std::lower_bound(std::begin(t), std::end(t), va, [](const engine::meta::NamedFunction& f, uint64_t v) { return f.va < v; });
    if (it != std::end(t) && it->va == va) return it->name;
    char b[32];
    snprintf(b, sizeof(b), "FUN_%llx", static_cast<unsigned long long>(va));
    return b;
}

// A return address as "0x14... (Function+0xNN)" (the function from the game's unwind data).
std::string callerText(uint64_t ret) {
    char b[160];
    DWORD64 base = 0;
    if (const RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(ret, &base, nullptr); f && base == mem::rva(0x140000000ull)) {
        const uint64_t fn = 0x140000000ull + f->BeginAddress, va = mem::va(ret);
        snprintf(b, sizeof(b), "0x%llx (%s+0x%llx)", static_cast<unsigned long long>(va), nameOf(fn).c_str(),
                 static_cast<unsigned long long>(va - fn));
        return b;
    }
    snprintf(b, sizeof(b), "%llx", static_cast<unsigned long long>(ret));
    return b;
}

// An argument: its type when it points at a reflected object, else the number.
std::string argText(uint64_t v) {
    char b[160];
    if (v > 0x10000 && v < 0x7fffffffffffull) {
        if (const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), v)) {
            snprintf(b, sizeof(b), "%llx %s", static_cast<unsigned long long>(v), t->name.c_str());
            return b;
        }
    }
    snprintf(b, sizeof(b), "%llx", static_cast<unsigned long long>(v));
    return b;
}

double secondsAgo(uint64_t when) {
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    return double(q.QuadPart - int64_t(when)) / double(f.QuadPart);
}

std::vector<Call> recent(const Trace& t, size_t n) {
    const uint64_t total = t.calls.load();
    const size_t have = static_cast<size_t>(std::min<uint64_t>(total, kKept));
    std::vector<Call> out;
    for (size_t i = 0; i < std::min(n, have); ++i) out.push_back(t.ring[(total - 1 - i) % kKept]);
    return out;
}

std::string g_status; // (render thread)
} // namespace

// Starts tracing a function (render thread). False with a reason in the status line.
bool add(const std::string& text) {
    const uint64_t va = lookup(text);
    if (!va) {
        g_status = "Not a function address (0x14...) or an annotated name: " + text;
        return false;
    }
    std::lock_guard<std::mutex> lock(g_m);
    for (const auto& t : g_traces)
        if (t->va == va) {
            t->paused = false;
            g_status = "Already traced: " + t->name;
            return true;
        }
    auto t = std::make_unique<Trace>();
    t->va = va;
    t->name = nameOf(va);
    t->stub = makeStub(t.get());
    if (!t->stub) {
        g_status = "No memory for the stub";
        return false;
    }
    if (hookFunction("GridNode tracer", va, t->stub, reinterpret_cast<void**>(t->original)) != GN_OK) {
        g_status = "The detour was refused (see the log): " + t->name;
        return false; // the stub's page stays: nothing points at it
    }
    GN_LOG("tracer: tracing %s (%llx)", t->name.c_str(), static_cast<unsigned long long>(va));
    g_status = "Tracing " + t->name;
    g_traces.push_back(std::move(t));
    return true;
}

// The Tracer window (render thread).
void draw(bool* open) {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 60, ImGui::GetFontSize() * 30), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Tracer", open)) {
        ImGui::End();
        return;
    }
    static char input[128] = {};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
    const bool enter = ImGui::InputTextWithHint("##fn", "0x14... or FUN_14... or an annotated name", input, sizeof(input),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Trace") || enter) add(input);
    if (!g_status.empty()) ImGui::TextDisabled("%s", g_status.c_str());
    ImGui::TextDisabled("Every call: the arguments (rcx, rdx, r8, r9, two on the stack, xmm0..3) and the caller. Return values are not traced.");
    static int selected = 0;
    std::lock_guard<std::mutex> lock(g_m);
    if (g_traces.empty()) {
        ImGui::End();
        return;
    }
    selected = std::min<int>(selected, static_cast<int>(g_traces.size()) - 1);
    for (int i = 0; i < static_cast<int>(g_traces.size()); ++i) {
        Trace& t = *g_traces[size_t(i)];
        ImGui::PushID(i);
        char label[200];
        snprintf(label, sizeof(label), "%s  %llu calls%s", t.name.c_str(), static_cast<unsigned long long>(t.calls.load()),
                 t.paused ? "  (paused)" : "");
        if (ImGui::Selectable(label, selected == i, 0, ImVec2(ImGui::GetFontSize() * 26, 0))) selected = i;
        ImGui::SameLine();
        bool p = t.paused;
        if (ImGui::Checkbox("Pause", &p)) t.paused = p;
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) t.calls = 0;
        ImGui::SameLine();
        if (ImGui::SmallButton("Log the last 50")) {
            const auto calls = recent(t, 50);
            GN_LOG("tracer: %s, the last %d of %llu calls (newest first):", t.name.c_str(), static_cast<int>(calls.size()),
                   static_cast<unsigned long long>(t.calls.load()));
            for (const Call& c : calls)
                GN_LOG("  -%.3f s  thread %u  from %s  rcx %s  rdx %s  r8 %s  r9 %s  [5] %llx [6] %llx  xmm %.4g %.4g %.4g %.4g", secondsAgo(c.when),
                       c.thread, callerText(c.caller).c_str(), argText(c.args[0]).c_str(), argText(c.args[1]).c_str(), argText(c.args[2]).c_str(),
                       argText(c.args[3]).c_str(), static_cast<unsigned long long>(c.args[4]), static_cast<unsigned long long>(c.args[5]),
                       c.floats[0], c.floats[1], c.floats[2], c.floats[3]);
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    const Trace& t = *g_traces[size_t(selected)];
    if (ImGui::BeginTable("calls", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("ago");
        ImGui::TableSetupColumn("thread");
        ImGui::TableSetupColumn("caller");
        ImGui::TableSetupColumn("rcx / rdx / r8 / r9");
        ImGui::TableSetupColumn("stack, xmm0..3");
        ImGui::TableHeadersRow();
        for (const Call& c : recent(t, 200)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%.2f s", secondsAgo(c.when));
            ImGui::TableNextColumn();
            ImGui::Text("%u", c.thread);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(callerText(c.caller).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s\n%s\n%s\n%s", argText(c.args[0]).c_str(), argText(c.args[1]).c_str(), argText(c.args[2]).c_str(), argText(c.args[3]).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%llx  %llx\n%.4g %.4g %.4g %.4g", static_cast<unsigned long long>(c.args[4]), static_cast<unsigned long long>(c.args[5]),
                        c.floats[0], c.floats[1], c.floats[2], c.floats[3]);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// Unloading: the stubs stop calling into GridNode (the detours themselves come out with every hook).
void release() { g_on = false; }

} // namespace gn::host::tracer
