// ANT's type registry and layouts (gridnode/ant_types.hpp). Layout of the structures read here, from
// ant_registerSchema (0x142c0c3c0), ant_buildClassLayout (0x142fb0f00), its field pass
// (0x142fb1880) and ant_computeSchemaCrc32 (0x142fb0800), read in Ghidra on 2026-11-05.

#include "gridnode/ant_types.hpp"

#include <cstring>

namespace gn::ant {
namespace {

constexpr int32_t kMinIndex = -3;     // __guid
constexpr int32_t kMaxIndex = 1024;   // far more fields than any ANT type has
constexpr uint32_t kMaxStrings = 1u << 16;

struct Header {
    int32_t minIndex, maxIndex;
    uint32_t size, alignment;
    uint32_t stringsOffset, stringsSize;
    uint8_t flag, pad[3];
    uint32_t id;
};
static_assert(sizeof(Header) == 0x20, "layout header");

struct Record {
    uint32_t typeId;
    uint32_t elementSize;
    uint32_t offset;
    uint32_t nameOffset; // from the string table
    uint16_t count;
    uint16_t flags;
    uint16_t alignment;
    uint16_t slot;
    uint64_t typeLayout;
};
static_assert(sizeof(Record) == 0x20, "layout record");

bool readHeader(const reflect::MemoryView& m, uint64_t layout, Header& h) {
    if (!layout || !m.read(layout, &h, sizeof(h))) return false;
    if (h.minIndex < kMinIndex || h.minIndex > 0 || h.maxIndex < -1 || h.maxIndex > kMaxIndex) return false;
    const uint32_t records = 0x20u + static_cast<uint32_t>(h.maxIndex - h.minIndex + 1) * 0x20u;
    return h.stringsOffset >= records && h.stringsSize > 0 && h.stringsSize < kMaxStrings;
}

std::string str(const reflect::MemoryView& m, const Header& h, uint64_t layout, uint32_t at) {
    if (at >= h.stringsSize) return {};
    return m.readCString(layout + h.stringsOffset + at, h.stringsSize - at);
}

} // namespace

uint32_t nameHash(const char* name) {
    uint32_t h = 0x811c9dc5u;
    if (!name) return h;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p) h = (h * 0x1000193u) ^ *p;
    return h;
}

bool readType(const reflect::MemoryView& m, uint64_t layout, Type& out) {
    Header h{};
    if (!readHeader(m, layout, h)) return false;
    out = Type{};
    out.layout = layout;
    out.name = str(m, h, layout, 1); // the string table starts with a 0, then the type's name
    out.id = h.id, out.size = h.size, out.alignment = h.alignment;
    out.minIndex = h.minIndex, out.maxIndex = h.maxIndex;
    return !out.name.empty();
}

bool readFields(const reflect::MemoryView& m, const Type& t, std::vector<Field>& out) {
    out.clear();
    Header h{};
    if (!readHeader(m, t.layout, h)) return false;
    for (int32_t i = h.minIndex; i <= h.maxIndex; ++i) {
        Record r{};
        if (!m.read(t.layout + 0x20 + static_cast<uint64_t>(i - h.minIndex) * 0x20, &r, sizeof(r))) return false;
        Field f;
        f.index = i;
        f.name = r.nameOffset ? str(m, h, t.layout, r.nameOffset) : std::string();
        f.typeLayout = r.typeLayout;
        f.typeId = r.typeId, f.elementSize = r.elementSize, f.offset = r.offset;
        f.count = r.count, f.flags = r.flags;
        Type ft;
        if (r.typeLayout && readType(m, r.typeLayout, ft)) f.typeName = ft.name;
        out.push_back(std::move(f));
    }
    return true;
}

std::vector<Entry> registry(const reflect::MemoryView& m, uint64_t registryObject, size_t max) {
    std::vector<Entry> out;
    uint64_t begin = 0, end = 0;
    if (!registryObject || !m.read(registryObject, begin) || !m.read(registryObject + 8, end) || end < begin || (end - begin) % 16) return out;
    const uint64_t n = (end - begin) / 16;
    for (uint64_t i = 0; i < n && out.size() < max; ++i) {
        uint32_t hash = 0;
        uint64_t schema = 0, layout = 0;
        if (!m.read(begin + i * 16, hash) || !m.read(begin + i * 16 + 8, schema) || !schema || !m.read(schema, layout) || !layout) continue;
        Entry e;
        e.hash = hash, e.schema = schema, e.layout = layout;
        uint64_t factory = 0;
        if (m.read(schema + 8, factory) && factory) m.read(factory, e.functions);
        out.push_back(e);
    }
    return out;
}

uint64_t findLayout(const reflect::MemoryView& m, uint64_t registryObject, const char* name) {
    if (!name || !*name) return 0;
    const uint32_t want = nameHash(name);
    uint64_t begin = 0, end = 0;
    if (!registryObject || !m.read(registryObject, begin) || !m.read(registryObject + 8, end) || end < begin || (end - begin) % 16) return 0;
    // The game's binary search (sorted by hash), then the name checked: a hash can collide.
    uint64_t lo = 0, hi = (end - begin) / 16;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2;
        uint32_t h = 0;
        if (!m.read(begin + mid * 16, h)) return 0;
        if (h < want) lo = mid + 1;
        else hi = mid;
    }
    uint32_t h = 0;
    uint64_t schema = 0, layout = 0;
    if (lo >= (end - begin) / 16 || !m.read(begin + lo * 16, h) || h != want || !m.read(begin + lo * 16 + 8, schema) || !schema ||
        !m.read(schema, layout))
        return 0;
    Type t;
    return readType(m, layout, t) && t.name == name ? layout : 0;
}

bool findField(const reflect::MemoryView& m, uint64_t layout, const char* name, Field& out, int depth) {
    if (!name || depth > 16) return false;
    Type t;
    std::vector<Field> fields;
    if (!readType(m, layout, t) || !readFields(m, t, fields)) return false;
    const Field* base = nullptr;
    for (const Field& f : fields) {
        if (f.index >= 0 && f.name == name) {
            out = f;
            return true;
        }
        if (f.index == -1 && f.typeLayout) base = &f;
    }
    if (!base) return false;
    Field inBase;
    if (!findField(m, base->typeLayout, name, inBase, depth + 1)) return false;
    out = inBase;
    out.offset += base->offset;
    return true;
}

} // namespace gn::ant
