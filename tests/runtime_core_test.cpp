// Host-side tests for the runtime's portable core: type registry, hook table, tick dispatcher.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "gridnode/ant_types.hpp"
#include "gridnode/audio_mix.hpp"
#include "gridnode/class_lists.hpp"
#include "gridnode/config.hpp"
#include "gridnode/field_diff.hpp"
#include "gridnode/dispatch.hpp"
#include "gridnode/entity_index.hpp"
#include "gridnode/hook_table.hpp"
#include "gridnode/mod.hpp"
#include "gridnode/mod_call.hpp"
#include "gridnode/mod_order.hpp"
#include "gridnode/near_alloc.hpp"
#include "gridnode/pe_file.hpp"
#include "gridnode/projection.hpp"
#include "gridnode/vhook_set.hpp"
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

    // typeOfLive: the same answer from real memory with plain loads.
    const uint64_t leafVtable[1] = {0x9100}, otherVtable[1] = {0x1234};
    const uint64_t* leafObject = leafVtable;
    const uint64_t* otherObject = otherVtable;
    const uint64_t* noVtable = nullptr;
    CHECK(reg.typeOfLive(&leafObject) == L);
    CHECK(reg.typeOfLive(&otherObject) == nullptr);
    CHECK(reg.typeOfLive(&noVtable) == nullptr);
    CHECK(reg.typeOfLive(nullptr) == nullptr);
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
                       [](uint64_t fn) { return fn >= 0xA0 && fn < 0x100; },
                       nullptr}); // no foreign hooks here
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
    // Two mods on one slot are reported; one mod (or GridNode's own parts) alone is not.
    {
        const auto isMod = [](const std::string& o) { return o != "gridnode"; };
        const auto sh = t.shared(isMod);
        CHECK(sh.size() == 1 && sh[0].slot == 0x1000 && sh[0].owners.size() == 2 && sh[0].owners[0] == "a" && sh[0].owners[1] == "b");
        CHECK(t.othersAt(0x1000, "b", isMod) == std::vector<std::string>{"a"});
        CHECK(t.othersAt(0x1008, "c", isMod).empty());
        CHECK(t.shared([](const std::string& o) { return o != "a"; }).empty());
    }

    // Taking hooks out (a mod unloaded). Top of the chain: the slot gets back what it held.
    std::map<uint64_t, uint64_t> m2{{0x3000, 0xA0}};
    hook::HookTable r({[&](uint64_t s, uint64_t& v) {
                           auto it = m2.find(s);
                           if (it == m2.end()) return false;
                           v = it->second;
                           return true;
                       },
                       [&](uint64_t s, uint64_t v) {
                           m2[s] = v;
                           return true;
                       },
                       [](uint64_t fn) { return fn >= 0xA0 && fn < 0x100; },
                       [](uint64_t fn) { return fn >= 0xF00 && fn < 0xF10; }});
    uint64_t o1 = 0, o2 = 0, o3 = 0;
    CHECK(r.install("gridnode", 0x3000, 0xA0, 0xD1, &o1) == hook::Result::Ok);
    CHECK(r.install("mod", 0x3000, 0, 0xD2, &o2) == hook::Result::Ok && o2 == 0xD1);
    CHECK(r.install("gridnode", 0x3000, 0, 0xD3, &o3) == hook::Result::Ok && o3 == 0xD2);
    // In the middle: the hook above it now calls what it called.
    std::vector<std::string> refused;
    CHECK(r.remove([](const hook::Entry& e) { return e.owner == "mod"; }, &refused) == 1 && refused.empty());
    CHECK(o3 == 0xD1 && m2[0x3000] == 0xD3 && r.count() == 2);
    // On top: the slot gets the hook below back.
    CHECK(r.remove([](const hook::Entry& e) { return e.detour == 0xD3; }) == 1 && m2[0x3000] == 0xD1 && r.count() == 1);
    // Another mod's hook on top of ours: ours stays (its original is not ours to change).
    m2[0x3000] = 0xF01;
    CHECK(r.remove([](const hook::Entry&) { return true; }, &refused) == 0 && refused.size() == 1 && r.count() == 1);
    // The last one out: the slot is the game's again.
    m2[0x3000] = 0xD1;
    CHECK(r.remove([](const hook::Entry&) { return true; }) == 1 && m2[0x3000] == 0xA0 && r.count() == 0);

    // A full unload: every owner at once, chains of several hooks on several slots, all back to the game's.
    m2[0x4000] = 0xB0;
    uint64_t p1 = 0, p2 = 0, p3 = 0, q1 = 0;
    CHECK(r.install("gridnode", 0x3000, 0xA0, 0xD1, &p1) == hook::Result::Ok);
    CHECK(r.install("mod", 0x3000, 0, 0xD2, &p2) == hook::Result::Ok);
    CHECK(r.install("recorder", 0x3000, 0, 0xD3, &p3) == hook::Result::Ok);
    CHECK(r.install("events", 0x4000, 0xB0, 0xD4, &q1) == hook::Result::Ok);
    refused.clear();
    CHECK(r.remove([](const hook::Entry&) { return true; }, &refused) == 4 && refused.empty());
    CHECK(m2[0x3000] == 0xA0 && m2[0x4000] == 0xB0 && r.count() == 0);
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

    // Listeners taken out (a mod unloaded): at the next tick, the others stay.
    thread::Dispatcher r;
    struct Tagged {
        int tag;
        int* calls;
        void operator()(float) const { ++calls[tag]; }
    };
    int calls[2] = {};
    r.addListener("a", Tagged{0, calls});
    r.addListener("b", Tagged{1, calls});
    r.dispatch(0.f);
    r.removeListeners([](const thread::Dispatcher::Tick& t) { const Tagged* g = t.target<Tagged>(); return g && g->tag == 1; });
    CHECK(r.removing());
    r.dispatch(0.f);
    CHECK(!r.removing() && calls[0] == 2 && calls[1] == 1 && r.listenerCount() == 1);
    // One not yet handed to the tick thread goes at once.
    r.addListener("c", Tagged{1, calls});
    r.removeListeners([](const thread::Dispatcher::Tick& t) { return t.target<Tagged>() != nullptr && t.target<Tagged>()->tag == 1; });
    r.dispatch(0.f);
    CHECK(calls[1] == 1 && calls[0] == 3);

    // Queued tasks of an unloading mod are dropped; the others still run.
    thread::Dispatcher q;
    struct Job {
        int tag;
        int* runs;
        void operator()() const { ++runs[tag]; }
    };
    int runs[2] = {};
    q.post(Job{0, runs});
    q.post(Job{1, runs});
    q.post(Job{1, runs});
    CHECK(q.removeTasks([](const thread::Dispatcher::Task& t) { const Job* j = t.target<Job>(); return j && j->tag == 1; }) == 2);
    CHECK(q.removing()); // until a whole tick has passed
    q.dispatch(0.f);
    CHECK(runs[0] == 1 && runs[1] == 0 && !q.removing());
    // The filter skips a task whose code is being unloaded, at the moment it would run.
    std::vector<std::string> qErrors;
    q.setErrorSink([&](const std::string& e) { qErrors.push_back(e); });
    q.setTaskFilter([](const thread::Dispatcher::Task& t) { const Job* j = t.target<Job>(); return !j || j->tag != 1; });
    q.post(Job{1, runs});
    q.post(Job{0, runs});
    q.dispatch(0.f);
    CHECK(runs[0] == 2 && runs[1] == 0 && qErrors.size() == 1);
    // removing() stays true while a tick that took tasks before the removal is still running them.
    thread::Dispatcher w;
    std::atomic<bool> inTask{false}, release{false};
    w.post([&] {
        inTask = true;
        while (!release) std::this_thread::yield();
    });
    std::thread ticker([&] { w.dispatch(0.f); });
    while (!inTask) std::this_thread::yield();
    w.removeTasks([](const thread::Dispatcher::Task&) { return true; });
    CHECK(w.removing());
    release = true;
    ticker.join();
    CHECK(w.removing()); // that tick started before the removal: the next one must pass first
    w.dispatch(0.f);
    CHECK(!w.removing());
}

// Byte-addressed memory made of blobs (ANT layouts are packed records, not 8-byte words).
class Bytes : public reflect::MemoryView {
public:
    std::map<uint64_t, std::vector<uint8_t>> blobs; // start -> bytes
    bool read(uint64_t addr, void* out, size_t size) const override {
        auto it = blobs.upper_bound(addr);
        if (it == blobs.begin()) return false;
        --it;
        const uint64_t off = addr - it->first;
        if (off + size > it->second.size()) return false;
        memcpy(out, it->second.data() + off, size);
        return true;
    }
};

// One ANT layout as ant_buildClassLayout writes it: header, records from minIndex, then names.
struct AntFieldSpec {
    int32_t index;
    const char* name;
    uint32_t offset;
    uint64_t typeLayout;
    uint16_t count;
    uint16_t flags;
};
std::vector<uint8_t> antLayout(const char* typeName, uint32_t size, uint32_t id, const std::vector<AntFieldSpec>& fields) {
    const int32_t minIndex = -3;
    int32_t maxIndex = -1;
    for (const auto& f : fields) maxIndex = std::max(maxIndex, f.index);
    const uint32_t stringsOffset = 0x20u + static_cast<uint32_t>(maxIndex - minIndex + 1) * 0x20u;
    std::string strings(1, '\0');
    strings += typeName;
    strings += '\0';
    std::vector<uint8_t> b(stringsOffset, 0);
    auto put32 = [&](size_t o, uint32_t v) { memcpy(&b[o], &v, 4); };
    auto put16 = [&](size_t o, uint16_t v) { memcpy(&b[o], &v, 2); };
    auto put64 = [&](size_t o, uint64_t v) { memcpy(&b[o], &v, 8); };
    put32(0, static_cast<uint32_t>(minIndex)), put32(4, static_cast<uint32_t>(maxIndex)), put32(8, size), put32(0xc, 4);
    put32(0x10, stringsOffset), put32(0x1c, id);
    for (const auto& f : fields) {
        const size_t r = 0x20 + static_cast<size_t>(f.index - minIndex) * 0x20;
        put32(r + 0x08, f.offset);
        if (f.name && *f.name) {
            put32(r + 0x0c, static_cast<uint32_t>(strings.size()));
            strings += f.name;
            strings += '\0';
        }
        put16(r + 0x10, f.count), put16(r + 0x12, f.flags);
        put64(r + 0x18, f.typeLayout);
    }
    put32(0x14, static_cast<uint32_t>(strings.size()));
    b.insert(b.end(), strings.begin(), strings.end());
    return b;
}

void testAntTypes() {
    CHECK(ant::nameHash("") == 0x811c9dc5u);
    CHECK(ant::nameHash("a") == ((0x811c9dc5u * 0x1000193u) ^ 'a')); // FNV-1: multiply, then xor
    Bytes m;
    constexpr uint64_t kFloat = 0x101000, kBase = 0x102000, kDriver = 0x103000, kOther = 0x103800; // above readCString's lowest address
    m.blobs[kFloat] = antLayout("Float", 4, 0xF10A7, {});
    m.blobs[kBase] = antLayout("DriverAsset", 0x10, 0xBA5E, {{-1, "__base", 0, 0, 1, 0}, {0, "Weight", 0x8, kFloat, 1, 0}});
    m.blobs[kDriver] = antLayout("FallingDriverAsset", 0x40, 0xFA11,
                                 {{-1, "__base", 0x0, kBase, 1, 0}, {0, "GravityY", 0x30, kFloat, 1, 1}, {1, "JumpHeight", 0x10, kFloat, 2, 0}});
    m.blobs[kOther] = antLayout("Other", 8, 0x0, {});
    // The registry: entries sorted by name hash, each {hash, pad, schema}; a schema's first word is the layout.
    std::vector<std::pair<uint32_t, uint64_t>> types = {{ant::nameHash("Float"), kFloat}, {ant::nameHash("DriverAsset"), kBase},
                                                       {ant::nameHash("FallingDriverAsset"), kDriver}, {ant::nameHash("Other"), kOther}};
    std::sort(types.begin(), types.end());
    std::vector<uint8_t> entries(types.size() * 16, 0), schemas(types.size() * 16, 0);
    constexpr uint64_t kEntries = 0x105000, kSchemas = 0x106000, kRegistry = 0x107000;
    for (size_t i = 0; i < types.size(); ++i) {
        const uint64_t schema = kSchemas + i * 16;
        memcpy(&entries[i * 16], &types[i].first, 4);
        memcpy(&entries[i * 16 + 8], &schema, 8);
        memcpy(&schemas[i * 16], &types[i].second, 8);
    }
    m.blobs[kEntries] = entries;
    m.blobs[kSchemas] = schemas;
    std::vector<uint8_t> reg(0x10, 0);
    const uint64_t end = kEntries + entries.size();
    memcpy(&reg[0], &kEntries, 8), memcpy(&reg[8], &end, 8);
    m.blobs[kRegistry] = reg;

    {
        const auto entries = ant::registry(m, kRegistry);
        CHECK(entries.size() == 4 && entries[0].layout && entries[0].schema >= kSchemas && entries[0].functions == 0); // no factory in the fake
    }
    CHECK(ant::findLayout(m, kRegistry, "FallingDriverAsset") == kDriver);
    CHECK(ant::findLayout(m, kRegistry, "DriverAsset") == kBase);
    CHECK(ant::findLayout(m, kRegistry, "NoSuchAsset") == 0);
    CHECK(ant::findLayout(m, kRegistry, nullptr) == 0);
    ant::Type t;
    CHECK(ant::readType(m, kDriver, t) && t.name == "FallingDriverAsset" && t.size == 0x40 && t.id == 0xFA11 && t.minIndex == -3 && t.maxIndex == 1);
    std::vector<ant::Field> fields;
    CHECK(ant::readFields(m, t, fields) && fields.size() == 5); // -3, -2, -1, 0, 1
    CHECK(fields[4].name == "JumpHeight" && fields[4].offset == 0x10 && fields[4].count == 2 && fields[4].typeName == "Float");
    CHECK(fields[3].name == "GravityY" && fields[3].flags == 1);
    ant::Field f;
    CHECK(ant::findField(m, kDriver, "GravityY", f) && f.offset == 0x30);
    CHECK(ant::findField(m, kDriver, "Weight", f) && f.offset == 0x8); // from the base, at __base's offset 0
    CHECK(!ant::findField(m, kDriver, "Nope", f));
    // A bad pointer or a corrupt header reads nothing (no huge index ranges).
    CHECK(!ant::readType(m, 0x109999, t));
    std::vector<uint8_t> bad = m.blobs[kOther];
    const int32_t hugeMax = 100000;
    memcpy(&bad[4], &hugeMax, 4);
    m.blobs[0x108000] = bad;
    CHECK(!ant::readType(m, 0x108000, t));
}

// A minimal PE32+ file with an export table naming the given functions.
std::vector<uint8_t> fakeDll(const std::vector<std::string>& names) {
    std::vector<uint8_t> f(0x400, 0);
    auto put16 = [&](size_t o, uint16_t v) { memcpy(&f[o], &v, 2); };
    auto put32 = [&](size_t o, uint32_t v) { memcpy(&f[o], &v, 4); };
    put16(0, 0x5a4d);
    put32(0x3c, 0x80);
    put32(0x80, 0x00004550);
    const size_t coff = 0x84, opt = coff + 20;
    put16(coff + 2, 1);   // one section
    put16(coff + 16, 240); // optional header size (PE32+)
    put16(opt, 0x20b);
    put32(opt + 108, 16);  // data directories
    put32(opt + 112, 0x1000); // export directory RVA
    put32(opt + 116, 0x100);
    const size_t sec = opt + 240;
    memcpy(&f[sec], ".edata", 6);
    put32(sec + 8, 0x200);  // virtual size
    put32(sec + 12, 0x1000); // virtual address
    put32(sec + 16, 0x200); // raw size
    put32(sec + 20, 0x200); // raw offset: RVA 0x1000 is file offset 0x200
    const size_t dir = 0x200;
    put32(dir + 24, static_cast<uint32_t>(names.size()));
    put32(dir + 32, 0x1040); // name pointers
    size_t str = 0x80;       // strings at RVA 0x1080
    for (size_t i = 0; i < names.size(); ++i) {
        put32(dir + 0x40 + i * 4, static_cast<uint32_t>(0x1000 + str));
        memcpy(&f[0x200 + str], names[i].c_str(), names[i].size() + 1);
        str += names[i].size() + 1;
    }
    return f;
}

void testPeExports() {
    const std::vector<uint8_t> dll = fakeDll({"gn_mod_info", "gn_mod_load", "gn_mod_unload"});
    CHECK(pe::exportsName(dll.data(), dll.size(), "gn_mod_unload"));
    CHECK(pe::exportsName(dll.data(), dll.size(), "gn_mod_load"));
    CHECK(!pe::exportsName(dll.data(), dll.size(), "gn_mod"));          // a prefix is not a name
    CHECK(!pe::exportsName(dll.data(), dll.size(), "gn_mod_unload_x"));
    const std::vector<uint8_t> noUnload = fakeDll({"gn_mod_load"});
    CHECK(!pe::exportsName(noUnload.data(), noUnload.size(), "gn_mod_unload"));
    // Truncated or not a PE: false, never a read past the end.
    for (size_t n : {size_t(0), size_t(2), size_t(0x90), size_t(0x210), size_t(0x2a0)}) CHECK(!pe::exportsName(dll.data(), n, "gn_mod_unload"));
    std::vector<uint8_t> bad = dll;
    bad[0] = 'X';
    CHECK(!pe::exportsName(bad.data(), bad.size(), "gn_mod_unload"));
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
    CHECK(config::fileName("MEC-Trainer") == "mec-trainer"); // one file on Windows either way
    CHECK(config::fileName("con") == "_con");
    CHECK(config::fileName("NUL.x") == "_nul.x");
    CHECK(config::fileName("com1") == "_com1");
    CHECK(config::fileName("console") == "console");
    {
        const std::string a = config::fileName(std::string(80, 'a') + "1"), b = config::fileName(std::string(80, 'a') + "2");
        CHECK(a.size() == 64 && b.size() == 64 && a != b);
    }
}

void testMovementEvents() {
    using namespace moveevents;
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

void testEventHash() {
    // Ids named in db/annotations/events.yaml (checked against the game there).
    CHECK(gn::eventHash("StartReplay") == 0x2734a476u);
    CHECK(gn::eventHash("StopReplay") == 0xd6a2182eu);
    CHECK(gn::eventHash("PlayerTriggeredSafeHouse") == 0x18c33d9au);
    static_assert(gn::eventHash("StreamOut") == 0x21f3f677u, "usable at compile time");
}

void testDeclaredSettings() {
    using namespace gn::config;
    Decl f;
    f.id = "speed", f.label = "Speed", f.description = "Metres per tick", f.kind = Kind::Float;
    f.def = 20, f.min = 10, f.max = 0.5; // backwards, default outside
    CHECK(normalize(f));
    CHECK(f.min == 0.5 && f.max == 10 && f.def == 10);
    CHECK(clamp(f, 99) == 10 && clamp(f, -1) == 0.5 && clamp(f, 2.25) == 2.25);
    CHECK(format(f, 2.25) == "2.25");
    CHECK(parseNumber(f, " 3 ") == 3 && parseNumber(f, "abc") == 10 && parseNumber(f, "") == 10 && parseNumber(f, "50") == 10);
    CHECK(comment(f) == "Speed: Metres per tick. From 0.5 to 10, default 10.");

    Decl i;
    i.id = "count", i.kind = Kind::Int, i.def = 3.6, i.min = 0, i.max = 5;
    CHECK(normalize(i) && i.def == 4);
    CHECK(format(i, 2.4) == "2" && parseNumber(i, "7") == 5);

    Decl b;
    b.id = "on", b.kind = Kind::Bool, b.def = 5;
    CHECK(normalize(b) && b.def == 1);
    CHECK(parseNumber(b, "false") == 0 && parseNumber(b, "true") == 1 && parseNumber(b, "0") == 0 && format(b, 0.2) == "1");

    Decl c;
    c.id = "mode", c.label = "Mode", c.kind = Kind::Choice, c.choices = {"off", "slow", "fast"}, c.def = 1;
    CHECK(normalize(c));
    CHECK(parseNumber(c, "fast") == 2 && parseNumber(c, "9") == 2 && parseNumber(c, "1") == 1);
    CHECK(comment(c) == "Mode. One of: 0 off, 1 slow, 2 fast. Default 1.");
    Decl none;
    none.kind = Kind::Choice;
    CHECK(!normalize(none));
    Decl bad;
    bad.kind = static_cast<Kind>(9);
    CHECK(!normalize(bad));

    const std::string text = serialize({{"speed", "2"}, {"other", "x"}}, "Head", {{"speed", "Speed: fast.\nSecond line."}});
    CHECK(text == "# Head\n\n# Speed: fast.\n# Second line.\nspeed=2\nother=x\n");
    CHECK(*find(parse(text), "speed") == "2"); // the comments read back as nothing
}

void testModOrder() {
    using namespace gn::modorder;
    const Dep d = parseDep(" ?trainer >= 1.2 ");
    CHECK(d.soft && d.id == "trainer" && d.minVersion == "1.2");
    CHECK(!parseDep("core").soft && parseDep("core").minVersion.empty());
    CHECK(compareVersions("1.2", "1.2.0") == 0);
    CHECK(compareVersions("1.10", "1.9") > 0);
    CHECK(compareVersions("2-beta", "2.0") == 0);
    CHECK(compareVersions("0.9", "1") < 0);

    // a needs c; b soft-needs a; c alone: c, a, b.
    std::vector<Entry> mods = {{"a", "1.0", {parseDep("c>=2")}}, {"b", "1", {parseDep("?a")}}, {"c", "2.1", {}}};
    Plan p = plan(mods);
    CHECK((p.order == std::vector<size_t>{2, 0, 1}));
    // c too old: a out, b still loads (soft).
    mods[2].version = "1.5";
    p = plan(mods);
    CHECK((p.order == std::vector<size_t>{2, 1}));
    CHECK(p.problem[0].find("2 or newer") != std::string::npos);
    // A missing soft dependency is fine; a missing hard one is not, and takes its dependents.
    p = plan({{"x", "", {parseDep("?nothere")}}, {"y", "", {parseDep("nothere")}}, {"z", "", {parseDep("y")}}});
    CHECK((p.order == std::vector<size_t>{0}));
    CHECK(p.problem[1].find("not there") != std::string::npos);
    CHECK(p.problem[2].find("did not load") != std::string::npos);
    // Two mods that optionally need each other both load (in file order); a soft link into a hard
    // cycle does not save the cycle.
    p = plan({{"s1", "", {parseDep("?s2")}}, {"s2", "", {parseDep("?s1")}}, {"after", "", {parseDep("s2")}}});
    CHECK((p.order == std::vector<size_t>{0, 1, 2}));
    // A cycle, and a duplicate id.
    p = plan({{"p", "", {parseDep("q")}}, {"q", "", {parseDep("p")}}, {"r", "", {}}, {"r", "", {}}});
    CHECK((p.order == std::vector<size_t>{2}));
    CHECK(p.problem[0] == "in a dependency cycle" && p.problem[1] == "in a dependency cycle");
    CHECK(p.problem[3].find("already taken") != std::string::npos);
}

// Field diff: changed fields named, changed bytes no field covers by offset, values formatted.
void testFieldDiff() {
    std::vector<uint8_t> a(0x20, 0), b(0x20, 0);
    const float one = 1.f, two = 2.5f;
    std::memcpy(a.data() + 0x4, &one, 4);
    std::memcpy(b.data() + 0x4, &two, 4); // the field "speed"
    b[0x11] = 7;                          // a byte of "flags" (2 bytes at 0x10)
    b[0x1c] = 1;                          // no field there
    const std::vector<gn::fielddiff::FieldSpan> fields = {{"speed", 0x4, 4}, {"unchanged", 0x8, 8}, {"flags", 0x10, 2}};
    const auto d = gn::fielddiff::diff(a, b, fields);
    CHECK(d.size() == 3);
    CHECK(d.size() == 3 && d[0].name == "speed" && d[1].name == "flags" && d[2].name == "+0x1c");
    CHECK(gn::fielddiff::format(b.data() + 0x4, 4) == "2.5");
    CHECK(gn::fielddiff::format(b.data() + 0x1c, 4) == "1");      // a small integer, not a float
    CHECK(gn::fielddiff::format(b.data() + 0x10, 2) == "1792");
    CHECK(gn::fielddiff::diff(a, a, fields).empty());
    // A pointer no field covers is one 8-byte change; a word that is a float on one side reads as a float on both.
    std::vector<uint8_t> c(0x20, 0), e(0x20, 0);
    const uint64_t p1 = 0x1be7b3060ull, p2 = 0x1a5906420ull;
    std::memcpy(c.data() + 0x18, &p1, 8), std::memcpy(e.data() + 0x18, &p2, 8);
    const float neg = -0.25f;
    std::memcpy(c.data(), &neg, 4);
    e[0] = 0x50, e[1] = 0xa2, e[2] = 0x0b, e[3] = 0x15; // 0x150ba250: an integer, or a float near 2.8e-26
    const auto d2 = gn::fielddiff::diff(c, e, {});
    CHECK(d2.size() == 2 && d2[0].offset == 0 && d2[0].size == 4 && d2[1].offset == 0x18 && d2[1].size == 8);
    CHECK(gn::fielddiff::format(e.data() + 0x18, 8) == "0x1a5906420");
    const auto both = gn::fielddiff::formatBoth(c.data(), e.data(), 4);
    CHECK(both.first == "0xbe800000" && both.second == "0x150ba250"); // not a float on one side: hex on both
    const float f1 = 1.5f, f2 = -471.033f;
    std::memcpy(c.data() + 8, &f1, 4), std::memcpy(e.data() + 8, &f2, 4);
    const auto floats = gn::fielddiff::formatBoth(c.data() + 8, e.data() + 8, 4);
    CHECK(floats.first == "1.5" && floats.second == "-471.033");
}

// Mixer offsets (audio_mix.hpp): sets add across mods, ducks ramp and the deepest wins, unload drops a mod's.
void testMixOffsets() {
    audio::MixOffsets m;
    float out[audio::kGroups];
    CHECK(m.step(0.f, out).empty());
    CHECK(out[4] == 0.f);

    m.set("a", 0x10, 4, 0.5f);
    m.set("b", 0x20, 4, 0.5f);
    std::vector<int> ch = m.step(0.f, out);
    CHECK(ch.size() == 1 && ch[0] == 4);
    CHECK(std::fabs(out[4] - 2.f * audio::gainToDb(0.5f)) < 1e-4f); // two mods add in dB
    m.set("a", 0x10, 4, 1.f);                                       // 1 = no change: removed
    m.step(0.f, out);
    CHECK(std::fabs(out[4] - audio::gainToDb(0.5f)) < 1e-4f);
    CHECK(audio::gainToDb(0.f) == audio::kFloorDb && audio::gainToDb(-1.f) == audio::kFloorDb && audio::gainToDb(2.f) == 0.f);

    // Duck: attack 0.1 s, hold, release 0.5 s; linear in gain, so halfway down is gain 0.625.
    audio::MixOffsets d;
    d.duck("a", 0x10, 7, 0.25f, 1.f);
    d.step(0.05f, out);
    CHECK(std::fabs(out[7] - audio::gainToDb(0.625f)) < 1e-3f);
    d.step(0.05f, out);
    CHECK(std::fabs(out[7] - audio::gainToDb(0.25f)) < 1e-3f);
    d.step(0.5f, out); // held
    CHECK(std::fabs(out[7] - audio::gainToDb(0.25f)) < 1e-3f);
    d.step(0.6f, out); // t = 1.2: 0.1 s into the 0.5 s release (the hold ends at 1.1)
    CHECK(out[7] > audio::gainToDb(0.25f) && out[7] < 0.f);
    CHECK(d.any());
    ch = d.step(0.5f, out); // over: back to 0, and the group is reported
    CHECK(out[7] == 0.f && !d.any());
    CHECK(!ch.empty() && ch.back() == 7);
    CHECK(d.step(0.1f, out).empty());

    // Ducks on one group do not add (the deepest wins); a set adds under them.
    audio::MixOffsets e;
    e.duck("a", 0x10, 3, 0.5f, 5.f);
    e.duck("b", 0x20, 3, 0.25f, 5.f);
    e.set("a", 0x10, 3, 0.5f);
    e.step(0.2f, out);
    CHECK(std::fabs(out[3] - (audio::gainToDb(0.25f) + audio::gainToDb(0.5f))) < 1e-3f);

    // A mod unloaded: its set and duck go, the other's stay; the groups come back for a recompute.
    const std::vector<int> gone = e.forget([](uintptr_t tag) { return tag == 0x10; });
    CHECK(!gone.empty());
    e.step(0.f, out);
    CHECK(std::fabs(out[3] - audio::gainToDb(0.25f)) < 1e-3f);
    e.forget([](uintptr_t) { return true; });
    e.step(0.f, out);
    CHECK(out[3] == 0.f && !e.any());
    m.set("a", 1, -1, 0.5f); // out-of-range groups are ignored
    m.set("a", 1, audio::kGroups, 0.5f);
}

// A fake address space answering as VirtualQuery does: a free region is described from the queried
// page upwards only.
struct FakeSpace {
    struct Block {
        uint64_t begin, end;
        bool free;
        uint64_t allocBase;
    };
    std::vector<Block> blocks; // sorted, covering everything that is asked about
    std::vector<uint64_t> tried;
    bool query(uint64_t a, nearmem::Region& r) const {
        for (const Block& b : blocks)
            if (a >= b.begin && a < b.end) {
                r.base = a & ~uint64_t(0xfff);
                r.size = b.end - r.base;
                r.free = b.free;
                r.allocationBase = b.free ? 0 : b.allocBase;
                return true;
            }
        return false;
    }
    // An allocation works anywhere wholly inside one free block.
    bool take(uint64_t a, uint64_t size) {
        tried.push_back(a);
        for (const Block& b : blocks)
            if (b.free && a >= b.begin && a + size <= b.end) return true;
        return false;
    }
};

void modCallbackA(void*) {}
void modCallbackB(void*) {}

void testModCall() {
    const char own[16] = {};  // stands in for gridnode.dll's code
    const char other[16] = {}; // a mod's code
    const auto inOwn = [&](const void* p) { return p >= own && p < own + sizeof(own); };
    CHECK(modcall::current() == nullptr);
    CHECK(modcall::resolve(own + 4, inOwn) == own + 4); // no mod callback running: as it is
    {
        modcall::Scope a(&modCallbackA);
        CHECK(modcall::resolve(own + 4, inOwn) == reinterpret_cast<const void*>(&modCallbackA)); // a tail call
        CHECK(modcall::resolve(other, inOwn) == other); // the mod's own return address wins
        {
            modcall::Scope b(&modCallbackB); // a callback inside a callback (entities_near's fn)
            CHECK(modcall::resolve(own, inOwn) == reinterpret_cast<const void*>(&modCallbackB));
        }
        CHECK(modcall::current() == reinterpret_cast<const void*>(&modCallbackA));
    }
    CHECK(modcall::current() == nullptr);
    std::thread([&] { CHECK(modcall::current() == nullptr); }).join(); // per thread
}

// hook_virtual's bookkeeping: a class and two subclasses, one overriding the slot.
void testVirtualHooks() {
    constexpr uint64_t kBase = 0x1000, kSub = 0x2000, kOver = 0x3000, kSlot = 0x18;
    std::map<uint64_t, uint64_t> mem{{kBase + kSlot, 0xA0}, {kSub + kSlot, 0xA0}, {kOver + kSlot, 0xB0}};
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
                       [](uint64_t fn) { return fn >= 0xA0 && fn < 0x100; }, nullptr});
    vhook::Set set("mod", {kOver, kBase, kSub, kBase}, kSlot, 0xD1); // unsorted, a duplicate
    CHECK(set.vtableCount() == 3);
    std::string why;
    CHECK(set.install(t, &why) == 3 && why.empty());
    CHECK(mem[kBase + kSlot] == 0xD1 && mem[kSub + kSlot] == 0xD1 && mem[kOver + kSlot] == 0xD1);
    // Each object reaches its own class's function.
    CHECK(set.original(kBase) == 0xA0 && set.original(kSub) == 0xA0 && set.original(kOver) == 0xB0);
    CHECK(set.original(0x4000) == 0); // a vtable it did not hook
    // A second hook on top: its originals are the first one's detour; the first's stay the game's.
    vhook::Set top("other", {kBase, kOver}, kSlot, 0xD2);
    CHECK(top.install(t) == 2 && top.original(kOver) == 0xD1 && set.original(kOver) == 0xB0);
    // The lower one taken out: the upper one now calls the game's function directly.
    CHECK(set.remove(t) == 3);
    CHECK(top.original(kOver) == 0xB0 && top.original(kBase) == 0xA0);
    CHECK(mem[kSub + kSlot] == 0xA0 && mem[kOver + kSlot] == 0xD2);
    CHECK(set.hookedCount() == 0 && set.original(kBase) == 0xA0); // still callable by a thread inside it
    CHECK(top.remove(t) == 2);
    CHECK(mem[kBase + kSlot] == 0xA0 && mem[kSub + kSlot] == 0xA0 && mem[kOver + kSlot] == 0xB0); // all restored
    // A slot holding no game function is refused; the rest still take.
    mem[0x5000 + kSlot] = 0x55;
    vhook::Set part("mod", {kBase, 0x5000}, kSlot, 0xD3);
    CHECK(part.install(t, &why) == 1 && !why.empty() && part.hookedCount() == 1);
    CHECK(part.remove(t) == 1 && mem[kBase + kSlot] == 0xA0);
}

void testEntityIndex() {
    struct T {
        int id;
    };
    const T base{1}, sub{2}, other{3};
    EntityIndex<T> ix(4);
    CHECK(ix.add(0x100, &base) && ix.add(0x200, &sub) && ix.add(0x300, &other) && ix.add(0x400, &sub));
    CHECK(!ix.add(0x500, &base) && ix.dropped() == 1); // full
    CHECK(ix.find(0x200) == &sub && ix.find(0x999) == nullptr);
    // By class: only the classes picked are read.
    auto subs = ix.ofClasses([&](const T* t) { return t == &sub; });
    std::sort(subs.begin(), subs.end());
    CHECK(subs.size() == 2 && subs[0].first == 0x200 && subs[1].first == 0x400);
    // Destroyed: gone from both lists at once; the moved entries keep working.
    CHECK(ix.remove(0x200) && !ix.remove(0x200));
    CHECK(ix.ofClasses([&](const T* t) { return t == &sub; }).size() == 1 && ix.find(0x400) == &sub);
    CHECK(ix.remove(0x100) && ix.size() == 2);
    // An address reused by a new entity of another class.
    CHECK(ix.add(0x300, &base) && ix.find(0x300) == &base && ix.ofClasses([&](const T* t) { return t == &other; }).empty());
    // removeIf only when it is still that entity.
    CHECK(!ix.removeIf(0x300, &other) && ix.removeIf(0x300, &base));
    // Counts and the walk in steps see every entity once.
    for (uintptr_t p = 0x1000; p < 0x1003; ++p) ix.add(p, &other);
    size_t total = 0;
    for (const auto& [t, n] : ix.counts()) total += n;
    CHECK(total == ix.size() && ix.size() == 4);
    size_t from = 0, seen = 0;
    do seen += ix.slice(from, 3).size();
    while (from != 0);
    CHECK(seen == 4 && ix.all().size() == 4);
}

// Finding the entities made before GridNode loaded, from the game's per-class lists.
void testClassLists() {
    std::map<uint64_t, uint64_t> mem;
    constexpr uint64_t kVtable = 0x9000, kOther = 0x9100, kClassInfo = 0x1000;
    // Three entities of the class with their node at +0x40, linked from the client realm's head.
    const uint64_t ents[3] = {0x20000, 0x30000, 0x40000};
    for (uint64_t e : ents) mem[e] = kVtable;
    mem[0x20000 + 0x18] = kOther; // a member that looks like a vtable of another class: not taken
    mem[kClassInfo + classlists::kHeadsAt] = ents[0] + 0x40;
    mem[ents[0] + 0x40] = ents[1] + 0x40;
    mem[ents[1] + 0x40] = ents[2] + 0x40;
    mem[ents[2] + 0x40] = 0;
    const auto read = [&](uint64_t a, uint64_t& v) {
        auto it = mem.find(a);
        v = it == mem.end() ? 0 : it->second;
        return true;
    };
    const auto isClass = [&](uint64_t a) { auto it = mem.find(a); return it != mem.end() && it->second == kVtable; };
    std::vector<uint64_t> got;
    uint32_t back = 0;
    size_t unresolved = 0;
    uint64_t heads[4];
    classlists::heads(kClassInfo, heads);
    CHECK(heads[0] == kClassInfo + 0x60 && heads[1] == kClassInfo + 0x80 && heads[2] == kClassInfo + 0x70 && heads[3] == kClassInfo + 0x90);
    size_t n = 0;
    for (uint64_t h : heads) n += classlists::walk(h, back, read, isClass, [&](uint64_t e) { got.push_back(e); }, 1000, unresolved);
    CHECK(n == 3 && back == 0x40 && unresolved == 0);
    CHECK(got.size() == 3 && got[0] == ents[0] && got[2] == ents[2]);
    // A node with no entity of the class behind it is counted, not taken; a cycle stops at the cap.
    mem[ents[2] + 0x40] = 0x50040;
    mem[0x50040] = ents[0] + 0x40;
    got.clear(), unresolved = 0;
    classlists::walk(heads[0], back, read, isClass, [&](uint64_t e) { got.push_back(e); }, 10, unresolved);
    CHECK(unresolved >= 1 && got.size() <= 10);
}

void testNearAlloc() {
    constexpr uint64_t kGran = 0x10000, kSize = 0x10000, kImage = 0x140000000;
    auto run = [&](FakeSpace& s, bool down, uint64_t from) {
        const nearmem::Query q = [&](uint64_t a, nearmem::Region& r) { return s.query(a, r); };
        const nearmem::TryAt t = [&](uint64_t a) { return s.take(a, kSize); };
        int calls = 0;
        const nearmem::Stop stop = [&] { return ++calls > 100000; };
        return down ? nearmem::searchDown(q, from, from - 0x7ff00000, kSize, kGran, 8, t, stop)
                    : nearmem::searchUp(q, from, from + 0x7ff00000, kSize, kGran, 8, t, stop);
    };
    { // A 64 KB gap just below the image, then taken memory below it (one allocation of many regions).
        FakeSpace s;
        s.blocks = {{0x0, kImage - 0x30000, false, 0x0}, {kImage - 0x30000, kImage - 0x20000, false, kImage - 0x30000},
                    {kImage - 0x20000, kImage - 0x10000, true, 0}, {kImage - 0x10000, kImage, false, kImage - 0x10000}};
        // From the page below the image, as patchesReserve asks: the gap's start is found.
        CHECK(run(s, true, kImage - 0x1000) == kImage - 0x20000);
        CHECK(s.tried.size() == 1);
    }
    { // A gap exactly one page long below the image: never tried, the walk goes on below it.
        FakeSpace s;
        s.blocks = {{0x0, 0x100000000, false, 0x0}, {0x100000000, 0x100100000, true, 0},
                    {0x100100000, kImage - 0x1000, false, 0x100100000}, {kImage - 0x1000, kImage, true, 0}};
        CHECK(run(s, true, kImage) == 0x100100000 - kSize); // the highest fit in the large free block
        for (uint64_t a : s.tried) CHECK(a < kImage - 0x1000);
    }
    { // No gap at all: nothing tried, and the walk ends.
        FakeSpace s;
        s.blocks = {{0x0, kImage, false, 0x0}};
        CHECK(run(s, true, kImage) == 0);
        CHECK(s.tried.empty());
    }
    { // A gap only above the image: down finds nothing, up finds its first aligned address.
        FakeSpace s;
        s.blocks = {{0x0, kImage + 0x4000000, false, 0x0}, {kImage + 0x4000000, kImage + 0x4000000 + 0x200000, true, 0},
                    {kImage + 0x4000000 + 0x200000, 0x7fffffffffff, false, kImage + 0x4000000 + 0x200000}};
        CHECK(run(s, true, kImage) == 0);
        CHECK(run(s, false, kImage + 0x1000000) == kImage + 0x4000000);
    }
    { // Free memory whose top is taken part-way into the request: the candidate below the taken part.
        FakeSpace s;
        s.blocks = {{0x0, 0x130000000, false, 0x0}, {0x130000000, kImage - 0x8000, true, 0}, {kImage - 0x8000, kImage, false, kImage - 0x8000}};
        CHECK(run(s, true, kImage) == kImage - 0x8000 - kSize - 0x8000); // aligned down below the taken 32 KB
    }
}

} // namespace

int main() {
    testNearAlloc();
    testModCall();
    testVirtualHooks();
    testEntityIndex();
    testClassLists();
    testFieldDiff();
    testModOrder();
    testDeclaredSettings();
    testRegistry();
    testHookTable();
    testDispatcher();
    testPeExports();
    testAntTypes();
    testConfig();
    testMovementEvents();
    testProjection();
    testEventHash();
    testMixOffsets();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("runtime_core_test: all checks passed");
    return 0;
}
