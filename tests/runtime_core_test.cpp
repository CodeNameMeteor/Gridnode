// Host-side tests for the runtime's portable core: type registry, hook table, tick dispatcher.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "gridnode/config.hpp"
#include "gridnode/dispatch.hpp"
#include "gridnode/hook_table.hpp"
#include "gridnode/projection.hpp"
#include "gridnode/reflect/registry.hpp"

using namespace gn;

static int g_failures = 0;
#define CHECK(cond)                                                                           \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                                     \
        }                                                                                     \
    } while (0)

namespace {

// Memory as a map of 8-byte words.
class Words : public reflect::MemoryView {
public:
    std::map<uint64_t, uint64_t> w;
    bool read(uint64_t addr, void* out, size_t size) const override {
        if (size != 8) return false;
        auto it = w.find(addr);
        if (it == w.end()) return false;
        memcpy(out, &it->second, 8);
        return true;
    }
};

reflect::Type cls(uint64_t ti, const char* name, uint64_t super, uint16_t id, uint16_t last) {
    reflect::Type t;
    t.ti = ti;
    t.name = name;
    t.category = reflect::Category::Class;
    t.superTi = super;
    t.classId = id;
    t.lastSubClassId = last;
    t.size = 0x40;
    return t;
}

void testRegistry() {
    reflect::Reflected r;
    reflect::Type boolean;
    boolean.ti = 0x10;
    boolean.name = "Boolean";
    boolean.category = reflect::Category::Primitive;
    boolean.size = 1;
    reflect::Type arr;
    arr.ti = 0x20;
    arr.category = reflect::Category::Array;
    arr.elementTi = 0x200;
    arr.size = 8;
    reflect::Type base = cls(0x100, "Base", 0, 1, 4);
    base.fields.push_back({"Enabled", 0, 0x10, 0x10});
    reflect::Type mid = cls(0x200, "Mid", 0x100, 2, 3);
    mid.fields.push_back({"Items", 0x48, 0x18, 0x20});
    mid.fields.push_back({"Enabled", 0, 0x20, 0x10}); // shadows Base.Enabled
    mid.typeAccessors.push_back(0x9000);
    reflect::Type leaf = cls(0x300, "Leaf", 0x200, 3, 3);
    leaf.typeAccessors.push_back(0x9100);
    reflect::Type other = cls(0x400, "Other", 0x100, 4, 4);
    for (auto* t : {&boolean, &arr, &base, &mid, &leaf, &other}) r.types.push_back(*t);
    for (size_t i = 0; i < r.types.size(); ++i) r.byTi[r.types[i].ti] = i;

    reflect::Registry reg;
    reg.build(std::move(r));
    const reflect::Type* B = reg.find("Base");
    const reflect::Type* M = reg.find("Mid");
    const reflect::Type* L = reg.find("Leaf");
    const reflect::Type* O = reg.find("Other");
    CHECK(B && M && L && O);
    CHECK(reg.find("Array<Mid>") == reg.byTi(0x20)); // unnamed arrays by display name
    CHECK(reg.isKindOf(L, M) && reg.isKindOf(L, B) && reg.isKindOf(M, M));
    CHECK(!reg.isKindOf(M, L) && !reg.isKindOf(O, M) && reg.isKindOf(O, B));
    CHECK(!reg.isKindOf(reg.find("Boolean"), B));

    reflect::FieldInfo fi;
    CHECK(reg.findField(L, "Items", fi) && fi.owner == M && fi.size == 8 && fi.field->offset == 0x18);
    CHECK(reg.findField(L, "Enabled", fi) && fi.owner == M && fi.field->offset == 0x20 && fi.size == 1);
    CHECK(reg.findField(B, "Enabled", fi) && fi.owner == B && fi.field->offset == 0x10);
    CHECK(!reg.findField(B, "Items", fi));

    Words m;
    m.w[0x5000] = 0x7000;   // object -> vtable
    m.w[0x7000] = 0x9100;   // vtable[0] = Leaf's accessor
    m.w[0x5100] = 0x7100;
    m.w[0x7100] = 0x1234;   // not an accessor
    CHECK(reg.typeOf(m, 0x5000) == L);
    CHECK(reg.typeOf(m, 0x5100) == nullptr);
    CHECK(reg.typeOf(m, 0x5200) == nullptr); // unreadable
    CHECK(reg.typeOf(m, 0) == nullptr);
}

void testHookTable() {
    std::map<uint64_t, uint64_t> mem{{0x1000, 0xA0}, {0x1008, 0xB0}, {0x1010, 0x55}};
    hook::HookTable t({[&](uint64_t s, uint64_t& v) {
                           auto it = mem.find(s);
                           if (it == mem.end()) return false;
                           v = it->second;
                           return true;
                       },
                       [&](uint64_t s, uint64_t v) {
                           mem[s] = v;
                           return true;
                       },
                       [](uint64_t fn) { return fn >= 0xA0 && fn < 0x100; }});
    uint64_t orig1 = 0, orig2 = 0, orig3 = 0;
    std::string why;
    CHECK(t.install("a", 0x1000, 0xA0, 0xD1, &orig1, &why) == hook::Result::Ok && orig1 == 0xA0 && mem[0x1000] == 0xD1);
    CHECK(t.install("b", 0x1000, 0xA0, 0xD2, &orig2, &why) == hook::Result::Ok && orig2 == 0xD1 && mem[0x1000] == 0xD2);
    // Expected value wrong on a fresh slot
    CHECK(t.install("c", 0x1008, 0xA0, 0xD3, &orig3, &why) == hook::Result::Slot);
    CHECK(mem[0x1008] == 0xB0);
    // expected = 0: any game function is fine, a non-function is not
    CHECK(t.install("c", 0x1008, 0, 0xD3, &orig3, &why) == hook::Result::Ok && orig3 == 0xB0);
    CHECK(t.install("c", 0x1010, 0, 0xD4, &orig3, &why) == hook::Result::Slot);
    // Changed behind GridNode's back: refused
    mem[0x1000] = 0xEE;
    CHECK(t.install("d", 0x1000, 0xA0, 0xD5, &orig3, &why) == hook::Result::Slot);
    CHECK(why.find("changed outside GridNode") != std::string::npos);
    // Another mod's hook (foreign code) in a fresh slot or on top of ours: chained, not refused
    hook::HookTable f({[&](uint64_t s, uint64_t& v) {
                           auto it = mem.find(s);
                           if (it == mem.end()) return false;
                           v = it->second;
                           return true;
                       },
                       [&](uint64_t s, uint64_t v) {
                           mem[s] = v;
                           return true;
                       },
                       [](uint64_t fn) { return fn >= 0xA0 && fn < 0x100; },
                       [](uint64_t fn) { return fn >= 0xF00 && fn < 0xF10; }});
    mem[0x1018] = 0xF01; // e.g. MECMP's tick hook, installed before GridNode
    uint64_t origF = 0;
    CHECK(f.install("g", 0x1018, 0xA0, 0xD6, &origF, &why) == hook::Result::Ok && origF == 0xF01 && mem[0x1018] == 0xD6);
    mem[0x1018] = 0xF02; // another mod went on top of GridNode's hook
    CHECK(f.install("g", 0x1018, 0xA0, 0xD7, &origF, &why) == hook::Result::Ok && origF == 0xF02 && mem[0x1018] == 0xD7);
    mem[0x1018] = 0x55; // not code at all: still refused
    CHECK(f.install("g", 0x1018, 0xA0, 0xD8, &origF, &why) == hook::Result::Slot);
    // Bad arguments
    CHECK(t.install("e", 0x1004, 0, 0xD6, &orig3, &why) == hook::Result::Argument); // misaligned
    CHECK(t.install("e", 0x2000, 0, 0xD6, &orig3, &why) == hook::Result::Slot);     // unreadable
    CHECK(t.count() == 3);
}

void testDispatcher() {
    thread::Dispatcher d;
    std::vector<std::string> errors;
    d.setErrorSink([&](const std::string& e) { errors.push_back(e); });
    std::vector<std::string> order;
    float lastDt = 0;
    d.addListener("a", [&](float dt) { order.push_back("a"); lastDt = dt; });
    d.addListener("bad", [&](float) { throw std::runtime_error("boom"); });
    d.post([&] { order.push_back("task"); });
    d.dispatch(0.25f);
    CHECK((order == std::vector<std::string>{"task", "a"}));
    CHECK(lastDt == 0.25f);
    CHECK(errors.size() == 1 && errors[0].find("bad") != std::string::npos);
    CHECK(d.listenerCount() == 1); // the throwing listener is gone
    order.clear();
    d.dispatch(0.1f);
    CHECK((order == std::vector<std::string>{"a"})); // tasks run once

    // Tasks posted from other threads all run, exactly once.
    std::atomic<int> ran{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back([&] {
            for (int k = 0; k < 250; ++k) d.post([&] { ++ran; });
        });
    std::atomic<bool> done{false};
    std::thread tick([&] {
        while (!done) d.dispatch(0.016f);
        d.dispatch(0.016f);
    });
    for (auto& t : threads) t.join();
    done = true;
    tick.join();
    CHECK(ran == 1000);
}

void testConfig() {
    // Comments, blank lines, spaces and lines without '=' are skipped; a later key wins.
    const config::Entries e = config::parse("# header\n\n bhop.below = 1.66 \r\nnoise\n;x=1\nkey.arm=F\nbhop.below=1.7\nempty=\n");
    CHECK(e.size() == 3);
    CHECK(config::find(e, "bhop.below") && *config::find(e, "bhop.below") == "1.7");
    CHECK(config::find(e, "key.arm") && *config::find(e, "key.arm") == "F");
    CHECK(config::find(e, "empty") && config::find(e, "empty")->empty());
    CHECK(!config::find(e, "x"));
    // Written and read back gives the same entries; the header lines are comments.
    const std::string text = config::serialize(e, "two\nlines");
    CHECK(text.rfind("# two\n# lines\n", 0) == 0);
    CHECK(config::parse(text) == e);
    // set: changed or not, and values on one line.
    config::Entries f;
    CHECK(config::set(f, "a", "1"));
    CHECK(!config::set(f, "a", " 1 "));
    CHECK(config::set(f, "a", "x\ny"));
    CHECK(*config::find(f, "a") == "x y");
    CHECK(config::validKey("key.arm") && !config::validKey("") && !config::validKey(" a") && !config::validKey("a=b") &&
          !config::validKey("#a") && !config::validKey("a\nb"));
    // File names stay inside the mods folder.
    CHECK(config::fileName("mec-trainer") == "mec-trainer");
    CHECK(config::fileName("../x y") == ".._x_y");
    CHECK(config::fileName("..") == "_..");
    CHECK(config::fileName("") == "_");
}

void testMovementEvents() {
    using namespace movement;
    // A bhop's landing tick: in the air in state 8 -> on the ground in state 0: landed, then the state.
    std::vector<Event> ev = diff({8, true}, {0, false});
    CHECK(ev.size() == 2);
    CHECK(ev[0].kind == kEventLanded && ev[0].state == 0 && ev[0].oldState == 8);
    CHECK(ev[1].kind == kEventState && ev[1].state == 0 && ev[1].oldState == 8);
    // Off again in the wall-run state.
    ev = diff({0, false}, {8, true});
    CHECK(ev.size() == 2 && ev[0].kind == kEventAirborne && ev[1].kind == kEventState);
    // Nothing changed: nothing.
    CHECK(diff({1, false}, {1, false}).empty());
    // A state change in the air alone.
    ev = diff({2, true}, {8, true});
    CHECK(ev.size() == 1 && ev[0].kind == kEventState && ev[0].oldState == 2);
}

void testProjection() {
    // At the origin looking along +z, 90 degrees high, a 200 x 100 screen.
    gn_view v{};
    v.forward[2] = 1.f, v.right[0] = 1.f, v.up[1] = 1.f;
    v.fov_y = 90.f, v.width = 200.f, v.height = 100.f;
    auto approx = [](float a, float b) { return a > b - 0.01f && a < b + 0.01f; };
    float s[2] = {};
    const float ahead[3] = {0.f, 0.f, 10.f}, right[3] = {10.f, 0.f, 10.f}, up[3] = {0.f, 10.f, 10.f}, behind[3] = {0.f, 0.f, -1.f};
    CHECK(projection::toScreen(v, ahead, s) && approx(s[0], 100.f) && approx(s[1], 50.f));
    CHECK(projection::toScreen(v, right, s) && approx(s[0], 150.f) && approx(s[1], 50.f)); // half the width (aspect 2)
    CHECK(projection::toScreen(v, up, s) && approx(s[0], 100.f) && approx(s[1], 0.f));     // the top edge, y down
    CHECK(!projection::toScreen(v, behind, s));
    CHECK(approx(projection::depth(v, ahead), 10.f) && projection::depth(v, behind) < 0.f);
    // A segment through the camera is cut at the near plane; one wholly behind is not drawn.
    float a[2] = {}, b[2] = {};
    const float from[3] = {1.f, 0.f, -5.f}, to[3] = {1.f, 0.f, 5.f}, back1[3] = {0.f, 0.f, -2.f}, back2[3] = {1.f, 1.f, -3.f};
    CHECK(projection::segmentToScreen(v, from, to, a, b));
    CHECK(approx(b[0], 110.f) && a[0] > 200.f); // the cut end is just in front of the camera: far off to the right
    CHECK(!projection::segmentToScreen(v, back1, back2, a, b));
}

} // namespace

int main() {
    testRegistry();
    testHookTable();
    testDispatcher();
    testConfig();
    testMovementEvents();
    testProjection();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("runtime_core_test: all checks passed");
    return 0;
}
