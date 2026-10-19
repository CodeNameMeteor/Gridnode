// Host-side test for the reflection walker: builds a small fake game image with the same
// layouts as the Catalyst PC build and checks what the walker reads back.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gridnode/reflect/dump_json.hpp"
#include "gridnode/reflect/layout.hpp"
#include "gridnode/reflect/walker.hpp"

using namespace gn::reflect;
namespace L = gn::layout;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

namespace {

constexpr uint64_t kBase = 0x140000000ull;
constexpr uint64_t kSize = 0x30000;

class FakeImage : public MemoryView {
public:
    FakeImage() : bytes_(kSize, 0) {}
    bool read(uint64_t addr, void* out, size_t size) const override {
        if (addr < kBase || addr + size > kBase + kSize || addr + size < addr) return false;
        for (uint64_t p = addr & ~0xfffull; p < addr + size; p += 0x1000)
            if (holes_.count(p)) return false;
        memcpy(out, &bytes_[addr - kBase], size);
        return true;
    }
    void put(uint64_t addr, const void* src, size_t n) { memcpy(&bytes_[addr - kBase], src, n); }
    template <class T> void put(uint64_t addr, T v) { put(addr, &v, sizeof(T)); }
    uint64_t str(const char* s) {
        const uint64_t a = strings_;
        put(a, s, strlen(s) + 1);
        strings_ += (strlen(s) + 16) & ~7ull;
        return a;
    }
    void hole(uint64_t page) { holes_.insert(page); }

private:
    std::vector<uint8_t> bytes_;
    std::set<uint64_t> holes_;
    uint64_t strings_ = kBase + 0x28000;
};

// Addresses in the fake image.
constexpr uint64_t kListHead = kBase + 0x10000;
constexpr uint64_t kModule = kBase + 0x27000;
constexpr uint64_t kCode = kBase + 0x1000, kCodeEnd = kBase + 0x4000;
constexpr uint64_t kData = kBase + 0x10000, kDataEnd = kBase + 0x20000;

uint64_t ti(int i) { return kBase + 0x12000 + uint64_t(i) * 0x80; }   // runtime TypeInfos
uint64_t tid(int i) { return kBase + 0x11000 + uint64_t(i) * 0x40; }  // static TypeInfoData
uint64_t fields(int i) { return kBase + 0x13000 + uint64_t(i) * 0x100; }

enum { Boolean, Vec3, DataContainer, MyData, MyEntity, ArrayMyData, MyEnum, Short, AfterShort, Hidden, Msg, Count };

void typeInfo(FakeImage& m, int i, const char* name, uint16_t flags, uint16_t size, uint64_t next) {
    m.put(ti(i) + L::TI_Data, tid(i));
    m.put(ti(i) + L::TI_Next, next);
    m.put<uint16_t>(ti(i) + L::TI_RuntimeId, uint16_t(100 + i));
    m.put(tid(i) + L::TID_Name, name ? m.str(name) : 0ull);
    m.put(tid(i) + L::TID_Flags, flags);
    m.put(tid(i) + L::TID_Size, size);
    m.put(tid(i) + L::TID_Module, kModule);
    m.put<uint8_t>(tid(i) + L::TID_Alignment, 8);
}

void field(FakeImage& m, uint64_t table, int idx, const char* name, uint16_t flags, uint16_t offset, uint64_t type) {
    const uint64_t e = table + uint64_t(idx) * L::FID_Stride;
    m.put(e + L::FID_Name, m.str(name));
    m.put(e + L::FID_Flags, flags);
    m.put(e + L::FID_Offset, offset);
    m.put(e + L::FID_Type, type);
}

// lea rax,[rip+disp]; ret  returning `target`
void accessor(FakeImage& m, uint64_t fn, uint64_t target) {
    uint8_t b[8] = {0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3};
    const int32_t disp = int32_t(int64_t(target) - int64_t(fn + 7));
    memcpy(b + 3, &disp, 4);
    m.put(fn, b, sizeof(b));
}

FakeImage build() {
    FakeImage m;
    m.put(kModule, m.str("Core"));
    // List (Hidden is not on it: it is only reachable as a field type).
    const int order[] = {Boolean, Vec3, DataContainer, MyData, MyEntity, ArrayMyData, MyEnum, Short, AfterShort, Msg};
    m.put(kListHead, ti(order[0]));
    const int n = int(sizeof(order) / sizeof(order[0]));
    for (int k = 0; k < n; ++k) {
        const int i = order[k];
        const uint64_t next = k + 1 < n ? ti(order[k + 1]) : 0;
        switch (i) {
        case Boolean: typeInfo(m, i, "Boolean", 0xad, 1, next); break;
        case Vec3: typeInfo(m, i, "Vec3", 0x29, 16, next); break;
        case DataContainer: typeInfo(m, i, "DataContainer", 0x35, 0x10, next); break;
        case MyData: typeInfo(m, i, "MyData", 0x35, 0x40, next); break;
        case MyEntity: typeInfo(m, i, "MyEntity", 0x35, 0x200, next); break;
        case ArrayMyData: typeInfo(m, i, nullptr, 0x41, 8, next); break;
        case MyEnum: typeInfo(m, i, "MyEnum", 0x89, 4, next); break;
        case Short: typeInfo(m, i, "ShortClass", 0x35, 8, next); break;
        case AfterShort: typeInfo(m, i, "AfterShort", 0x35, 8, next); break;
        case Msg: typeInfo(m, i, "MyMessage", 0x8029, 0x10, next); break;
        }
    }
    typeInfo(m, Hidden, "Hidden", 0x29, 4, 0);

    // Vec3 fields
    m.put<uint16_t>(tid(Vec3) + L::TID_FieldCount, 3);
    m.put(tid(Vec3) + L::TID_ValueTypeFields, fields(Vec3));
    field(m, fields(Vec3), 0, "x", 0, 0, ti(Boolean));
    field(m, fields(Vec3), 1, "y", 0, 4, ti(Boolean));
    field(m, fields(Vec3), 2, "z", 0, 8, ti(Boolean));

    // DataContainer: root class
    m.put<uint16_t>(ti(DataContainer) + L::CI_ClassId, 1);
    m.put<uint16_t>(ti(DataContainer) + L::CI_LastSubClassId, 2);

    // MyData : DataContainer, with a vtable at ClassInfo+0x50
    m.put(ti(MyData) + L::CI_Super, ti(DataContainer));
    m.put<uint16_t>(ti(MyData) + L::CI_ClassId, 2);
    m.put<uint16_t>(ti(MyData) + L::CI_LastSubClassId, 2);
    m.put(tid(MyData) + L::TID_ClassCreate, kCode + 0x100);
    m.put<uint16_t>(tid(MyData) + L::TID_FieldCount, 4);
    m.put(tid(MyData) + L::TID_ClassFields, fields(MyData));
    field(m, fields(MyData), 0, "Enabled", 0, 0x18, ti(Boolean));
    field(m, fields(MyData), 1, "Position", 0, 0x20, ti(Vec3));
    field(m, fields(MyData), 2, "Items", 0x48, 0x30, ti(ArrayMyData));
    field(m, fields(MyData), 3, "Secret", 0, 0x38, ti(Hidden));
    const uint64_t vtData = kBase + 0x14000;
    accessor(m, kCode + 0x10, ti(MyData));
    m.put(vtData, kCode + 0x10);
    m.put(ti(MyData) + L::CI_Slot50, vtData);

    // MyEntity: runtime class with a client and a server creator, vtable found by data scan.
    // Its accessor sits after an unreadable code page (exercises the page-by-page fallback).
    m.hole(kBase + 0x2000);
    accessor(m, kCode + 0x2010, ti(MyEntity));
    const uint64_t vtEntity = kBase + 0x14100;
    m.put(vtEntity, kCode + 0x2010);
    const uint64_t creatorVt = kBase + 0x14200;
    m.put(creatorVt, kCode + 0x300);
    for (uint32_t realm = 0; realm < 2; ++realm) {
        const uint64_t c = kBase + 0x15000 + realm * 0x40;
        m.put(c + L::Creator_Vtable, creatorVt);
        m.put(c + L::Creator_DataTi, ti(MyData));
        m.put(c + L::Creator_Realm, realm);
        m.put(ti(MyEntity) + (realm ? L::CI_Slot58 : L::CI_Slot50), c);
    }
    // A creator with the wrong realm in the wrong slot is rejected.
    m.put(kBase + 0x15100 + L::Creator_Vtable, creatorVt);
    m.put(kBase + 0x15100 + L::Creator_DataTi, ti(MyData));
    m.put<uint32_t>(kBase + 0x15100 + L::Creator_Realm, 1);
    m.put(ti(DataContainer) + L::CI_Slot50, kBase + 0x15100);

    // MyMessage: a message value type. Its accessor loads the TypeInfo through a pointer
    // (mov rax,[rip+p]; ret) and its static instance starts with its vtable.
    {
        const uint64_t fn = kCode + 0x40, ptr = kBase + 0x16000;
        m.put(ptr, ti(Msg));
        uint8_t b[8] = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3};
        const int32_t disp = int32_t(int64_t(ptr) - int64_t(fn + 7));
        memcpy(b + 3, &disp, 4);
        m.put(fn, b, sizeof(b));
        const uint64_t vt = kBase + 0x14300, inst = kBase + 0x16100;
        m.put(vt, fn);
        m.put(inst, vt);
        m.put(tid(Msg) + L::TID_ValueTypeInstance, inst);
    }
    // A class with two vtables sharing one accessor (a subclass without reflection of its own).
    accessor(m, kCode + 0x60, ti(AfterShort));
    m.put(kBase + 0x14400, kCode + 0x60);
    m.put(kBase + 0x14500, kCode + 0x60);

    // Array<MyData>
    m.put(tid(ArrayMyData) + L::TID_ArrayElement, ti(MyData));

    // MyEnum values
    m.put<uint16_t>(tid(MyEnum) + L::TID_FieldCount, 3);
    m.put(tid(MyEnum) + L::TID_EnumFields, fields(MyEnum));
    field(m, fields(MyEnum), 0, "MyEnum_A", 0, 0, 0);
    field(m, fields(MyEnum), 1, "MyEnum_B", 0, 0, 5);
    field(m, fields(MyEnum), 2, "MyEnum_C", 0, 0, 0xffffffffull);

    // ShortClass: the next record (a TypeInfoData placed by hand) starts 0x50 after it, so its
    // +0x50/+0x58 slots belong to the neighbour and must not be read.
    m.put(ti(Short) + L::CI_Slot50, vtData); // would wrongly confirm if read
    return m;
}

} // namespace

int main() {
    FakeImage m = build();
    // Make ShortClass short: put AfterShort's TypeInfoData right after it.
    // (TypeInfoData addresses count as record starts.)
    {
        // Re-point AfterShort's data to ti(Short)+0x50 and copy its record there.
        uint8_t rec[0x20]; // header only: the next TypeInfo starts 0x30 later
        m.read(tid(AfterShort), rec, sizeof(rec));
        m.put(ti(Short) + 0x50, rec, sizeof(rec));
        m.put(ti(AfterShort) + L::TI_Data, ti(Short) + 0x50);
    }

    CHECK(countTypes(m, kListHead) == 10);

    WalkOptions opt;
    opt.listHead = kListHead;
    opt.regions = {{kCode, kCodeEnd, true}, {kData, kDataEnd, false}};
    Reflected r = walk(m, opt);

    CHECK(r.stats.listed == 10);
    CHECK(r.types.size() == 11); // + Hidden, found through MyData's field
    for (size_t i = 1; i < r.types.size(); ++i) CHECK(r.types[i - 1].ti < r.types[i].ti);

    const Type* b = r.findByName("Boolean");
    CHECK(b && b->category == Category::Primitive && b->size == 1 && b->module == "Core" && b->runtimeId == 100);

    const Type* v = r.findByName("Vec3");
    CHECK(v && v->category == Category::ValueType && v->fields.size() == 3 && v->fields[2].offset == 8);

    const Type* d = r.findByName("MyData");
    CHECK(d && d->category == Category::Class);
    CHECK(d && d->superTi == ti(DataContainer) && d->classId == 2 && d->createFn == kCode + 0x100);
    CHECK(d && d->fields.size() == 4 && d->fields[2].flags == 0x48 && d->fields[2].typeTi == ti(ArrayMyData));
    CHECK(d && d->vtableConfirmed && d->vtables.size() == 1 && d->vtables[0] == kBase + 0x14000);
    CHECK(d && d->getType == kCode + 0x10);
    CHECK(d && d->creators.empty());

    const Type* e = r.findByName("MyEntity");
    CHECK(e && e->creators.size() == 2);
    CHECK(e && e->creators.size() == 2 && e->creators[0].realm == 0 && e->creators[1].realm == 1 &&
          e->creators[1].dataTi == ti(MyData));
    CHECK(e && !e->vtableConfirmed && e->vtables.size() == 1 && e->vtables[0] == kBase + 0x14100);
    CHECK(e && e->getType == kCode + 0x2010);

    const Type* dc = r.findByName("DataContainer");
    CHECK(dc && dc->creators.empty() && dc->vtables.empty()); // realm-mismatched creator rejected

    const Type* a = r.find(ti(ArrayMyData));
    CHECK(a && a->category == Category::Array && a->elementTi == ti(MyData));
    CHECK(r.displayName(ti(ArrayMyData)) == "Array<MyData>");

    const Type* en = r.findByName("MyEnum");
    CHECK(en && en->values.size() == 3 && en->values[1].value == 5 && en->values[2].value == -1);

    const Type* sh = r.findByName("ShortClass");
    CHECK(sh && sh->vtables.empty() && !sh->vtableConfirmed);
    CHECK(r.stats.shortClassInfos == 1);

    const Type* h = r.findByName("Hidden");
    CHECK(h && h->category == Category::ValueType);

    const Type* msg = r.findByName("MyMessage");
    CHECK(msg && msg->category == Category::ValueType && msg->vtableConfirmed && msg->vtables.size() == 1 &&
          msg->vtables[0] == kBase + 0x14300 && msg->getType == kCode + 0x40 && msg->staticInstance == kBase + 0x16100);
    CHECK(h && h->staticInstance == 0);

    const Type* as = r.findByName("AfterShort");
    CHECK(as && as->vtables.size() == 2 && !as->vtableConfirmed && as->getType == kCode + 0x60);

    CHECK(r.stats.accessors == 4);
    CHECK(r.stats.creators == 2);
    CHECK(r.stats.vtablesFromClassInfo == 1 && r.stats.vtablesFromStaticInstance == 1);
    CHECK(r.stats.vtablesFromDataScan == 1 && r.stats.vtableCandidatesAmbiguous == 1);

    // JSON: image VAs, no heap addresses, valid structure markers.
    DumpInfo info;
    info.image = {"Fake.exe", kBase, 0x140000000ull, kSize, 0x5762e3b7};
    info.generator = "test";
    std::ostringstream os;
    writeJson(os, r, info);
    const std::string js = os.str();
    CHECK(js.find("\"format\":\"gridnode-reflected\"") != std::string::npos);
    CHECK(js.find("\"displayName\":\"Array<MyData>\"") != std::string::npos);
    CHECK(js.find("\"timestamp\":\"0x5762e3b7\"") != std::string::npos);
    CHECK(js.find("\"getType\":\"0x140001010\"") != std::string::npos);

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("reflect_walker_test: all checks passed");
    return 0;
}
