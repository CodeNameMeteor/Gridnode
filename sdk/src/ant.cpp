// ANT's types for mods (gn_api ant_*, API 25) and Faith's move tracker (gn_api move_tracker).
//
// ANT (the animation and movement system) keeps its own type registry, separate from Frostbite's
// TypeInfos: every asset, driver, processor and value type by name, with a layout of its fields
// (gridnode/ant_types.hpp; docs/research/ant.md). It is read once, on the first ask or the first
// client tick that finds it complete, and kept: ANT registers its types at startup and never again.
//
// A layout's offsets are within the type's data. A live asset object has a header before that data
// (its vtable and more): measured here from two assets whose field places are known from Ghidra
// (RayCastCollisionProcessorAsset.FallThreshold at +0x40, PositionPredictionProcessorAsset.Timespan
// at +0x10), and given to mods as instance_offset only when both agree.

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gridnode/ant_types.hpp"
#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

namespace e = gn::engine;

struct Cache {
    bool ok = false;
    std::vector<ant::Type> types;                // by name
    std::vector<std::vector<ant::Field>> fields; // same order: each type's declared fields (index >= 0)
    std::vector<int64_t> base;                   // same order: the base type's index (-1: none)
    std::vector<uint32_t> baseOffset;            // where the base's data sits in this type's
    std::vector<uint64_t> functions;             // same order: the type's function table (create +0, load +8)
    std::unordered_map<std::string, size_t> byName;
    int32_t header = -1;                         // a live asset's data starts this far in (-1: not known)
};
std::mutex g_m;
Cache g_cache; // (g_m) built once; never changed after ok

uint64_t registryObject() {
    uint64_t p = 0;
    return mem::read(mem::rva(e::global::AntTypeRegistryPtr), p) ? p : 0;
}

const ant::Field* field(const Cache& c, const std::string& type, const char* name) {
    const auto it = c.byName.find(type);
    if (it == c.byName.end()) return nullptr;
    for (const ant::Field& f : c.fields[it->second])
        if (f.name == name) return &f;
    return nullptr;
}

// (g_m) Reads the registry; true once it is complete enough to keep.
bool buildLocked() {
    if (g_cache.ok) return true;
    const uint64_t reg = registryObject();
    if (!reg) return false;
    const mem::ProcessMemory m;
    const auto entries = ant::registry(m, reg);
    if (entries.size() < 100) return false; // not all registered yet (the game is starting)
    Cache c;
    for (const ant::Entry& entry : entries) {
        const uint64_t layout = entry.layout;
        ant::Type t;
        std::vector<ant::Field> all;
        if (!ant::readType(m, layout, t) || !ant::readFields(m, t, all)) continue;
        std::vector<ant::Field> own;
        uint64_t baseLayout = 0;
        uint32_t baseOffset = 0;
        for (ant::Field& f : all) {
            if (f.index == -1) baseLayout = f.typeLayout, baseOffset = f.offset;
            if (f.index >= 0) own.push_back(std::move(f));
        }
        c.functions.push_back(entry.functions);
        c.types.push_back(std::move(t));
        c.fields.push_back(std::move(own));
        c.base.push_back(static_cast<int64_t>(baseLayout)); // a layout for now; an index below
        c.baseOffset.push_back(baseOffset);
    }
    std::vector<size_t> order(c.types.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return c.types[a].name < c.types[b].name; });
    Cache sorted;
    std::unordered_map<uint64_t, size_t> byLayout;
    for (size_t i : order) {
        byLayout.emplace(c.types[i].layout, sorted.types.size());
        sorted.byName.emplace(c.types[i].name, sorted.types.size());
        sorted.types.push_back(std::move(c.types[i]));
        sorted.fields.push_back(std::move(c.fields[i]));
        sorted.functions.push_back(c.functions[i]);
        sorted.base.push_back(c.base[i]);
        sorted.baseOffset.push_back(c.baseOffset[i]);
    }
    // __base names a registered type when the type derives from one; otherwise (a placeholder type,
    // or none) the type has no base here.
    for (int64_t& b : sorted.base) {
        const auto it = byLayout.find(static_cast<uint64_t>(b));
        b = it != byLayout.end() ? static_cast<int64_t>(it->second) : -1;
    }
    // The asset header, from two fields whose places in the live asset are known.
    struct Known {
        const char* type;
        const char* field;
        int32_t at;
    };
    const Known known[] = {{"RayCastCollisionProcessorAsset", "FallThreshold", 0x40}, {"PositionPredictionProcessorAsset", "Timespan", 0x10}};
    int32_t header = -1;
    bool agree = true;
    for (const Known& k : known) {
        const ant::Field* f = field(sorted, k.type, k.field);
        const int32_t d = f ? k.at - static_cast<int32_t>(f->offset) : -1;
        GN_LOG("ant: %s.%s is at %s0x%x in its data (at +0x%x in a live asset)", k.type, k.field, f ? "" : "? ", f ? f->offset : 0u,
               static_cast<unsigned>(k.at));
        if (d < 0 || (header >= 0 && d != header)) agree = false;
        else header = d;
    }
    sorted.header = agree ? header : -1;
    sorted.ok = true;
    size_t fieldCount = 0;
    for (const auto& f : sorted.fields) fieldCount += f.size();
    GN_LOG("ant: %llu types, %llu fields read from the registry at %llx; a live asset's data starts at %s", static_cast<unsigned long long>(sorted.types.size()),
           static_cast<unsigned long long>(fieldCount), static_cast<unsigned long long>(reg),
           sorted.header >= 0 ? ("+0x" + std::to_string(sorted.header)).c_str() : "an unknown place (the two checks disagree)");
    g_cache = std::move(sorted);
    return true;
}

bool ready() {
    std::lock_guard<std::mutex> lock(g_m);
    return buildLocked();
}

bool kindOf(uintptr_t p, const char* base) {
    const reflect::Type* t = p ? registry().typeOf(mem::ProcessMemory(), p) : nullptr;
    const reflect::Type* b = registry().find(base);
    return t && b && registry().isKindOf(t, b);
}

template <class T> bool sized(const T* out) { return out && out->size >= sizeof(uint32_t) * 2; }

void fillType(const Cache& c, size_t i, gn_ant_type* out) {
    gn_ant_type v{};
    v.size = sizeof(v);
    v.id = c.types[i].id;
    v.name = c.types[i].name.c_str();
    v.data_size = c.types[i].size;
    v.alignment = c.types[i].alignment;
    v.field_count = static_cast<uint32_t>(c.fields[i].size());
    v.layout = reinterpret_cast<const void*>(static_cast<uintptr_t>(c.types[i].layout));
    const uint32_t n = std::min<uint32_t>(out->size, sizeof(v));
    memcpy(out, &v, n);
    out->size = n;
}

void fillField(const Cache& c, const ant::Field& f, gn_ant_field* out) {
    gn_ant_field v{};
    v.size = sizeof(v);
    v.index = f.index;
    v.name = f.name.c_str();
    v.type_name = f.typeName.c_str();
    v.offset = f.offset;
    v.instance_offset = c.header >= 0 ? static_cast<int32_t>(f.offset) + c.header : -1;
    v.element_size = f.elementSize;
    v.count = f.count;
    v.flags = f.flags;
    v.type_id = f.typeId;
    const uint32_t n = std::min<uint32_t>(out->size, sizeof(v));
    memcpy(out, &v, n);
    out->size = n;
}

} // namespace

void antTick() {
    static int tries = 0;
    if (tries < 0) return;
    if (++tries % 60 != 1) return; // every 2 s or so until it is read
    if (ready()) tries = -1;
}

int antTypeCount() { return ready() ? static_cast<int>(g_cache.types.size()) : -GN_ERR_UNAVAILABLE; }

int antTypeAt(uint32_t i, gn_ant_type* out) {
    if (!sized(out)) return GN_ERR_ARGUMENT;
    if (!ready()) return GN_ERR_UNAVAILABLE;
    if (i >= g_cache.types.size()) return GN_ERR_NOT_FOUND;
    fillType(g_cache, i, out);
    return GN_OK;
}

int antTypeFind(const char* name, gn_ant_type* out) {
    if (!name || !sized(out)) return GN_ERR_ARGUMENT;
    if (!ready()) return GN_ERR_UNAVAILABLE;
    const auto it = g_cache.byName.find(name);
    if (it == g_cache.byName.end()) return GN_ERR_NOT_FOUND;
    fillType(g_cache, it->second, out);
    return GN_OK;
}

int antFieldAt(const char* type, uint32_t i, gn_ant_field* out) {
    if (!type || !sized(out)) return GN_ERR_ARGUMENT;
    if (!ready()) return GN_ERR_UNAVAILABLE;
    const auto it = g_cache.byName.find(type);
    if (it == g_cache.byName.end() || i >= g_cache.fields[it->second].size()) return GN_ERR_NOT_FOUND;
    fillField(g_cache, g_cache.fields[it->second][i], out);
    return GN_OK;
}

int antFieldFind(const char* type, const char* name, gn_ant_field* out) {
    if (!type || !name || !sized(out)) return GN_ERR_ARGUMENT;
    if (!ready()) return GN_ERR_UNAVAILABLE;
    const auto it = g_cache.byName.find(type);
    if (it == g_cache.byName.end()) return GN_ERR_NOT_FOUND;
    // In the type, then its bases (each base's data sits at its __base offset).
    uint32_t add = 0;
    for (int64_t t = static_cast<int64_t>(it->second), depth = 0; t >= 0 && depth < 16; ++depth) {
        for (const ant::Field& f : g_cache.fields[static_cast<size_t>(t)])
            if (f.name == name) {
                ant::Field at = f;
                at.offset += add;
                fillField(g_cache, at, out);
                return GN_OK;
            }
        add += g_cache.baseOffset[static_cast<size_t>(t)];
        t = g_cache.base[static_cast<size_t>(t)];
    }
    return GN_ERR_NOT_FOUND;
}

// The type list (a filter) or one type's fields (its exact name).
std::string typeListing(const std::string& a) {
    if (const auto it = g_cache.byName.find(a); !a.empty() && it != g_cache.byName.end()) {
        const ant::Type& t = g_cache.types[it->second];
        char line[256];
        snprintf(line, sizeof(line), "%s: id %08x, data %u bytes, %llu fields", t.name.c_str(), t.id, t.size,
                 static_cast<unsigned long long>(g_cache.fields[it->second].size()));
        std::string out = line;
        if (const int64_t b = g_cache.base[it->second]; b >= 0) out += ", base " + g_cache.types[static_cast<size_t>(b)].name;
        out += "\n";
        std::vector<ant::Field> fields = g_cache.fields[it->second];
        std::sort(fields.begin(), fields.end(), [](const ant::Field& x, const ant::Field& y) { return x.offset < y.offset; });
        for (const ant::Field& f : fields) {
            char inst[16] = "?";
            if (g_cache.header >= 0) snprintf(inst, sizeof(inst), "+0x%x", f.offset + static_cast<uint32_t>(g_cache.header));
            snprintf(line, sizeof(line), "  +0x%-4x (asset %s)  %-36s %s%s%s\n", f.offset, inst, f.name.c_str(), f.typeName.empty() ? "?" : f.typeName.c_str(),
                     f.count > 1 ? ("[" + std::to_string(f.count) + "]").c_str() : "", (f.flags & 1) ? "  (reference)" : "");
            out += line;
        }
        return out;
    }
    std::string filter = a;
    for (char& c : filter) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    std::string out;
    size_t n = 0;
    for (const ant::Type& t : g_cache.types) {
        std::string lower = t.name;
        for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
        if (n++ < 300) out += t.name + "\n";
    }
    out += std::to_string(n) + " ANT type(s)" + (filter.empty() ? "" : " matching \"" + a + "\"") + (n > 300 ? " (the first 300 shown)" : "") +
           ". ant NAME lists a type's fields.";
    return out;
}

// ---- live assets: where each field goes, which objects are of the type, reading and writing ----------
// A layout describes ANT's data for a type, not the C++ object the game runs with. Each type's
// function table (Schema +8 -> table) has its create at +0x00 and its load at +0x08: load(?, object,
// view, resolver) takes each field from the data (through 0x142fb0110: view +0x10 the layout, view
// +0x1c the data's offset from the view) and either copies it into a fixed place in the object or
// hands it to resolver->vtable[1](resolver, object, fieldData, dest, typeHash, 0), a reference
// (FallingDriverAsset's loader 0x142eee0b0, read in Ghidra 2026-11-05). So GridNode makes one object
// with the type's create and runs its load on test data: once with zeros, then once per field with
// only that field set; the bytes that change are the field's place, and a resolver call names a
// reference's place. Objects of the type are then found by the vtable create gave (a memory scan).
// Research tools for the console (client thread, where ANT reads its assets); not in the API yet.
namespace {

struct LiveField {
    std::string name;
    int32_t at = -1;        // in the object; -1: not found
    uint32_t size = 0;      // bytes written there (a copied value)
    bool reference = false; // resolved through the resolver: a pointer at `at`
    uint32_t refType = 0;   // the reference's type hash
};
struct LiveType {
    bool mapped = false, ok = false;
    std::string why;
    uint64_t vtable = 0;
    std::vector<LiveField> fields;
    std::vector<uintptr_t> objects; // found by the scan
};
std::unordered_map<size_t, LiveType> g_live; // client thread

bool inGameCode(uint64_t a) {
    if (!mem::inImage(static_cast<uintptr_t>(a))) return false;
    for (const mem::Section& s : mem::imageSections())
        if (s.region.code && a >= s.region.begin && a < s.region.end) return true;
    return false;
}

// The resolver the test loads get: it records each call and points the reference at zeros (a load
// that reads through a reference reads zeros, not a null pointer).
struct ResolverCall {
    uintptr_t fieldData, dest;
    uint32_t type;
};
std::vector<ResolverCall> g_calls;
alignas(16) uint8_t g_zeros[0x400];
uint64_t resolveRecord(void*, void*, void* fieldData, void** dest, uint32_t type, uint64_t) {
    if (g_calls.size() < 4096) g_calls.push_back({reinterpret_cast<uintptr_t>(fieldData), reinterpret_cast<uintptr_t>(dest), type});
    if (dest) *dest = g_zeros;
    return 1;
}
uint64_t resolveStub() { return 0; }
struct Resolver {
    void* const* vtable;
};
void* const g_resolverTable[8] = {reinterpret_cast<void*>(&resolveStub), reinterpret_cast<void*>(&resolveRecord), reinterpret_cast<void*>(&resolveStub),
                                  reinterpret_cast<void*>(&resolveStub), reinterpret_cast<void*>(&resolveStub), reinterpret_cast<void*>(&resolveStub),
                                  reinterpret_cast<void*>(&resolveStub), reinterpret_cast<void*>(&resolveStub)};

constexpr size_t kObjectWindow = 0x400; // bytes of the object compared (objects are smaller; reading past is harmless)
constexpr uint32_t kViewData = 0x40;    // the data's offset from the view

LiveType& mapType(size_t i) {
    LiveType& lt = g_live[i];
    if (lt.mapped) return lt;
    lt.mapped = true;
    const ant::Type& t = g_cache.types[i];
    const uint64_t fn = g_cache.functions[i];
    uint64_t create = 0, load = 0;
    if (!fn || !mem::read(fn, create) || !mem::read(fn + 8, load) || !inGameCode(create) || !inGameCode(load)) {
        lt.why = "its create / load functions are not in the game's code";
        return lt;
    }
    // The loader reads the layout through the view, with no checks: a layout that is no longer what was
    // read (its memory freed or reused after startup; a crash on 2026-11-06: the first word held
    // 0x3c003816, not -3) sends it into wild memory. Look again first, and look up the live layout.
    {
        const mem::ProcessMemory m;
        ant::Type now;
        if (!ant::readType(m, t.layout, now) || now.name != t.name || now.id != t.id) {
            int32_t first = 0;
            mem::read(t.layout, first);
            GN_LOG("ant: %s: its layout at %llx is not the one read at startup any more (first word %x, now \"%s\"); not run", t.name.c_str(),
                   static_cast<unsigned long long>(t.layout), static_cast<unsigned>(first), now.name.c_str());
            lt.why = "its layout in memory has changed since ANT was read (see the log)";
            return lt;
        }
    }
    GN_LOG("ant: mapping %s: create %llx, load %llx, run on test data (a crash now is in its own loader)", t.name.c_str(),
           static_cast<unsigned long long>(mem::va(create)), static_cast<unsigned long long>(mem::va(load)));
    const uintptr_t obj = reinterpret_cast<uintptr_t>(reinterpret_cast<void* (*)(void*)>(create)(nullptr)); // kept: a few hundred bytes
    if (!obj || !mem::read(obj, lt.vtable) || !lt.vtable) {
        lt.why = "its create made nothing";
        return lt;
    }
    std::vector<uint8_t> view(kViewData + t.size + 0x40, 0);
    memcpy(view.data() + 0x10, &t.layout, 8);
    const uint16_t dataAt = kViewData;
    memcpy(view.data() + 0x1c, &dataAt, 2);
    uint8_t* data = view.data() + kViewData;
    Resolver resolver{g_resolverTable};
    using LoadFn = uint64_t (*)(void*, void*, void*, void*);
    auto run = [&](std::vector<uint8_t>& after) {
        g_calls.clear();
        reinterpret_cast<LoadFn>(load)(nullptr, reinterpret_cast<void*>(obj), view.data(), &resolver);
        after.assign(kObjectWindow, 0);
        mem::read(obj, after.data(), after.size());
    };
    const std::vector<ant::Field>& own = g_cache.fields[i];
    std::vector<uint8_t> base;
    run(base);
    const std::vector<ResolverCall> baseCalls = g_calls;
    for (const ant::Field& f : own) {
        LiveField lf;
        lf.name = f.name;
        const uintptr_t at = reinterpret_cast<uintptr_t>(data) + f.offset;
        for (const ResolverCall& c : baseCalls)
            if (c.fieldData == at && c.dest >= obj && c.dest < obj + kObjectWindow) {
                lf.reference = true, lf.at = static_cast<int32_t>(c.dest - obj), lf.refType = c.type, lf.size = 8;
                break;
            }
        if (!lf.reference) {
            const uint32_t n = std::max<uint32_t>(1, std::min<uint32_t>(f.elementSize ? f.elementSize * std::max<uint16_t>(f.count, 1) : 4, 64));
            // 8-byte fields are often pointers or offsets into the asset (strings, arrays): a test
            // pattern there would be read through, so they are left unplaced.
            const bool pointerLike = f.elementSize == 8 || (f.flags & 1) != 0 || f.index < 0;
            if (!pointerLike && f.offset + n <= t.size) {
                for (uint32_t k = 0; k < n; ++k) data[f.offset + k] = static_cast<uint8_t>(n == 1 ? 1 : 0x41 + k);
                std::vector<uint8_t> after;
                run(after);
                memset(data + f.offset, 0, n);
                int32_t first = -1, last = -1;
                for (size_t k = 0; k < kObjectWindow; ++k)
                    if (after[k] != base[k]) {
                        if (first < 0) first = static_cast<int32_t>(k);
                        last = static_cast<int32_t>(k);
                    }
                if (first >= 0) lf.at = first, lf.size = static_cast<uint32_t>(last - first + 1);
            }
        }
        lt.fields.push_back(lf);
    }
    run(base); // leave the object as loaded from zeros
    lt.ok = true;
    int found = 0;
    for (const LiveField& f : lt.fields) found += f.at >= 0 ? 1 : 0;
    GN_LOG("ant: %s mapped: vtable %llx, %d of %llu fields placed", t.name.c_str(), static_cast<unsigned long long>(mem::va(lt.vtable)), found,
           static_cast<unsigned long long>(lt.fields.size()));
    for (const LiveField& f : lt.fields) {
        if (f.at < 0) GN_LOG("ant:   %-36s not found", f.name.c_str());
        else GN_LOG("ant:   %-36s %s at +0x%x (%u bytes)", f.name.c_str(), f.reference ? "reference" : "value", static_cast<unsigned>(f.at), f.size);
    }
    return lt;
}

// Objects whose first word is the type's vtable, in the game's private read-write memory. A few
// seconds for a few GB; the client thread waits (a research command).
void findObjects(LiveType& lt) {
    lt.objects.clear();
    const ULONGLONG start = GetTickCount64();
    std::vector<uint64_t> buf(1u << 17); // 1 MB
    uint64_t scanned = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t a = 0x10000; a < 0x7ff000000000ull && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)); a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (mbi.Protect & PAGE_GUARD)) continue;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = begin + mbi.RegionSize;
        for (uintptr_t p = begin; p < end; p += buf.size() * 8) {
            const size_t n = static_cast<size_t>(std::min<uintptr_t>(buf.size() * 8, end - p));
            if (!mem::read(p, buf.data(), n)) continue;
            scanned += n;
            for (size_t k = 0; k < n / 8; k += 2) // objects are 16-byte aligned
                if (buf[k] == lt.vtable && lt.objects.size() < 256) lt.objects.push_back(p + k * 8);
        }
        if (GetTickCount64() - start > 15000) break;
    }
    GN_LOG("ant: %llu object(s) with vtable %llx in %llu MB, %llu ms", static_cast<unsigned long long>(lt.objects.size()),
           static_cast<unsigned long long>(mem::va(lt.vtable)), static_cast<unsigned long long>(scanned >> 20),
           static_cast<unsigned long long>(GetTickCount64() - start));
}

bool intType(const std::string& typeName) {
    std::string l = typeName;
    for (char& c : l) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return l.find("int") != std::string::npos;
}

std::string hexBytes(uintptr_t p, size_t n) {
    std::string out;
    uint8_t b[64] = {};
    if (!mem::read(p, b, std::min(n, sizeof(b)))) return "unreadable";
    char h[4];
    for (size_t i = 0; i < std::min(n, sizeof(b)); ++i) snprintf(h, sizeof(h), "%02x", b[i]), out += h, out += (i % 8 == 7 ? " " : "");
    return out;
}

// `ant TYPE FIELD [VALUE]`: the field in every object of the type, or set to VALUE.
std::string fieldCommand(size_t ti, const std::string& fieldName, const std::string& value) {
    LiveType& lt = mapType(ti);
    if (!lt.ok) return "Cannot map " + g_cache.types[ti].name + ": " + lt.why;
    const LiveField* lf = nullptr;
    const ant::Field* af = nullptr;
    for (size_t k = 0; k < lt.fields.size(); ++k)
        if (lt.fields[k].name == fieldName) lf = &lt.fields[k], af = &g_cache.fields[ti][k];
    if (!lf) return g_cache.types[ti].name + " has no field " + fieldName + " (ant " + g_cache.types[ti].name + " lists them)";
    if (lf->at < 0) return fieldName + ": its place in the live object was not found (see the ant: lines in the log)";
    if (lt.objects.empty() || std::any_of(lt.objects.begin(), lt.objects.end(), [&](uintptr_t o) { return mem::readOr<uint64_t>(o) != lt.vtable; }))
        findObjects(lt);
    if (lt.objects.empty()) return "No live " + g_cache.types[ti].name + " found (is its level loaded?)";
    const bool isInt = intType(af->typeName);
    std::string out;
    char line[256];
    if (!value.empty()) {
        if (lf->reference)
            return fieldName + " is a reference to another asset (at +0x" + [&] {
                char b[16];
                snprintf(b, sizeof(b), "%x", static_cast<unsigned>(lf->at));
                return std::string(b);
            }() + "), not a value: setting it is not supported yet. ant " + g_cache.types[ti].name + " " + fieldName + " shows what it points at.";
        char* end = nullptr;
        const double v = strtod(value.c_str(), &end);
        if (!end || *end) return "Not a number: " + value;
        int n = 0;
        for (uintptr_t o : lt.objects) {
            const uintptr_t at = o + static_cast<uint32_t>(lf->at);
            bool ok = false;
            if (lf->size == 1) ok = mem::write<uint8_t>(at, v != 0.0 ? 1 : 0);
            else if (lf->size == 4 && isInt) ok = mem::write<int32_t>(at, static_cast<int32_t>(v));
            else if (lf->size == 4) ok = mem::write<float>(at, static_cast<float>(v));
            else return fieldName + " is " + std::to_string(lf->size) + " bytes: only 1-byte (bool) and 4-byte (float, int) values can be set";
            n += ok ? 1 : 0;
        }
        GN_LOG("ant: %s.%s = %s in %d object(s)", g_cache.types[ti].name.c_str(), fieldName.c_str(), value.c_str(), n);
        snprintf(line, sizeof(line), "%s.%s set to %s in %d of %llu object(s). Drivers may only read it when a move starts.", g_cache.types[ti].name.c_str(),
                 fieldName.c_str(), value.c_str(), n, static_cast<unsigned long long>(lt.objects.size()));
        return line;
    }
    for (size_t k = 0; k < lt.objects.size() && k < 16; ++k) {
        const uintptr_t at = lt.objects[k] + static_cast<uint32_t>(lf->at);
        if (lf->reference) {
            const uintptr_t target = mem::readOr<uintptr_t>(at);
            const uintptr_t tvt = target ? mem::readOr<uintptr_t>(target) : 0;
            snprintf(line, sizeof(line), "  %llx: -> %llx (vtable %llx): %s\n", static_cast<unsigned long long>(lt.objects[k]), static_cast<unsigned long long>(target),
                     static_cast<unsigned long long>(tvt && mem::inImage(tvt) ? mem::va(tvt) : tvt), target ? hexBytes(target, 48).c_str() : "-");
        } else if (lf->size == 1) {
            snprintf(line, sizeof(line), "  %llx: %d\n", static_cast<unsigned long long>(lt.objects[k]), mem::readOr<uint8_t>(at));
        } else if (lf->size == 4) {
            snprintf(line, sizeof(line), "  %llx: %g (int %d)\n", static_cast<unsigned long long>(lt.objects[k]), mem::readOr<float>(at), mem::readOr<int32_t>(at));
        } else {
            snprintf(line, sizeof(line), "  %llx: %s\n", static_cast<unsigned long long>(lt.objects[k]), hexBytes(at, lf->size).c_str());
        }
        out += line;
    }
    snprintf(line, sizeof(line), "%s.%s (%s at +0x%x, %u bytes) in %llu object(s)%s", g_cache.types[ti].name.c_str(), fieldName.c_str(),
             lf->reference ? "a reference" : "a value", static_cast<unsigned>(lf->at), lf->size, static_cast<unsigned long long>(lt.objects.size()),
             lt.objects.size() > 16 ? " (the first 16 shown)" : "");
    GN_LOG("ant: %s\n%s", line, out.c_str());
    return std::string(line) + "\n" + out;
}

} // namespace

// The console's "ant":
//   ant [FILTER]              the types (a part of the name, case ignored)
//   ant TYPE                  its fields: offset in ANT's data, type
//   ant TYPE FIELD            the field in every live object of the type (maps and finds them first)
//   ant TYPE FIELD VALUE      sets it (copied values: float, int, bool)
std::string antCommand(const std::string& args) {
    if (!ready()) return "ANT's types are not read yet (the game is starting)";
    std::vector<std::string> words;
    for (size_t i = 0; i < args.size();) {
        const size_t a = args.find_first_not_of(' ', i);
        if (a == std::string::npos) break;
        const size_t b = args.find(' ', a);
        words.push_back(args.substr(a, b == std::string::npos ? std::string::npos : b - a));
        i = b == std::string::npos ? args.size() : b;
    }
    if (words.size() >= 2) {
        const auto it = g_cache.byName.find(words[0]);
        if (it == g_cache.byName.end()) return "No ANT type named " + words[0] + " (ant " + words[0] + " lists the ones matching)";
        return fieldCommand(it->second, words[1], words.size() >= 3 ? words[2] : std::string());
    }
    return typeListing(words.empty() ? std::string() : words[0]);
}

// ---- Faith's move tracker ---------------------------------------------------------------------------
// API 26: the probe for mods. Client thread (the type's own create and load run there, as for the
// console). The first call for a type maps it (and logs what it found); objects are scanned for when
// the last scan's are gone.
int antLiveField(const char* type, const char* field, gn_ant_live_field* out) {
    if (!type || !field || !out || out->size < sizeof(gn_ant_live_field)) return GN_ERR_ARGUMENT;
    if (!isClientThread()) return GN_ERR_THREAD;
    if (!ready()) return GN_ERR_UNAVAILABLE;
    const auto it = g_cache.byName.find(type);
    if (it == g_cache.byName.end()) return GN_ERR_NOT_FOUND;
    const LiveType& lt = mapType(it->second);
    if (!lt.ok) return GN_ERR_UNAVAILABLE;
    for (const LiveField& f : lt.fields) {
        if (f.name != field) continue;
        const uint32_t size = out->size;
        *out = gn_ant_live_field{};
        out->size = size;
        out->offset = f.at;
        out->bytes = f.size;
        out->is_reference = f.reference ? 1 : 0;
        out->reference_type = f.refType;
        out->vtable = reinterpret_cast<void*>(static_cast<uintptr_t>(lt.vtable));
        return GN_OK;
    }
    return GN_ERR_NOT_FOUND;
}

int antObjects(const char* type, void** out, uint32_t max) {
    if (!type || (!out && max)) return -GN_ERR_ARGUMENT;
    if (!isClientThread()) return -GN_ERR_THREAD;
    if (!ready()) return -GN_ERR_UNAVAILABLE;
    const auto it = g_cache.byName.find(type);
    if (it == g_cache.byName.end()) return -GN_ERR_NOT_FOUND;
    LiveType& lt = mapType(it->second);
    if (!lt.ok) return -GN_ERR_UNAVAILABLE;
    if (lt.objects.empty() || std::any_of(lt.objects.begin(), lt.objects.end(), [&](uintptr_t o) { return mem::readOr<uint64_t>(o) != lt.vtable; }))
        findObjects(lt);
    for (uint32_t i = 0; i < max && i < lt.objects.size(); ++i) out[i] = reinterpret_cast<void*>(lt.objects[i]);
    return static_cast<int>(lt.objects.size());
}

int moveTracker(gn_move_tracker* out) {
    if (!sized(out)) return GN_ERR_ARGUMENT;
    if (!isClientThread()) return GN_ERR_THREAD;
    namespace k = e::cls::PamClientMoveTrackerComponent::off;
    void* faith = localClientCharacter();
    static const reflect::Type* type = registry().find("PamClientMoveTrackerComponent");
    if (!faith || !type) return GN_ERR_UNAVAILABLE;
    const uintptr_t c = reinterpret_cast<uintptr_t>(getComponent(faith, reinterpret_cast<const gn_type*>(type)));
    if (!c || !kindOf(c, "PamClientMoveTrackerComponent") || !mem::readOr<uintptr_t>(c + k::animatable)) return GN_ERR_UNAVAILABLE;
    gn_move_tracker v{};
    v.size = sizeof(v);
    v.current_move = mem::readOr<int32_t>(c + k::move, -1);
    v.damage_type = mem::readOr<int32_t>(c + k::damageType, -1);
    v.in_jump_state = mem::readOr<uint8_t>(c + k::inJumpState) ? 1 : 0;
    v.moving_speed_xz = mem::readOr<float>(c + k::movingSpeedXZ);
    v.moved_this_tick = mem::readOr<float>(c + k::movedThisTick);
    float p[4] = {}, n[4] = {};
    mem::read(c + k::antMovePosition, p, sizeof(p));
    mem::read(c + k::antMoveNormal, n, sizeof(n));
    for (int i = 0; i < 3; ++i) v.move_position[i] = p[i], v.move_normal[i] = n[i];
    const uint32_t size = std::min<uint32_t>(out->size, sizeof(v));
    memcpy(out, &v, size);
    out->size = size;
    return GN_OK;
}

} // namespace gn::host
