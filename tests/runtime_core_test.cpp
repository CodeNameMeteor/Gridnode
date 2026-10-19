// Host-side tests for the runtime's portable core: type registry, hook table, tick dispatcher.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "gridnode/dispatch.hpp"
#include "gridnode/hook_table.hpp"
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

} // namespace

int main() {
    testRegistry();
    testHookTable();
    testDispatcher();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("runtime_core_test: all checks passed");
    return 0;
}
