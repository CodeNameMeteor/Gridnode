#pragma once
// ANT's own reflection: the animation system (EA ANT, Pamplona's movement) registers every asset,
// driver and value type by name with a layout of its fields, separate from Frostbite's TypeInfos.
// Read from the game's registry (docs/research/ant.md). Portable: all memory access goes through
// reflect::MemoryView, so it is tested on the host (tests/runtime_core_test.cpp).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "gridnode/reflect/walker.hpp"

namespace gn::ant {

// ant_hashString_fnv1 (0x143fbc460, mode 0): FNV-1 (multiply, then xor), seed 0x811c9dc5.
uint32_t nameHash(const char* name);

// A type's layout (ant_buildClassLayout 0x142fb0f00): a 0x20-byte header, then one 0x20-byte record
// per field index from minIndex (-3: __guid, -2: __name, -1: __base) to maxIndex, then the names.
struct Type {
    uint64_t layout = 0;
    std::string name;
    uint32_t id = 0;        // the layout's CRC-32 (header +0x1c): ANT's type id
    uint32_t size = 0;      // header +0x08
    uint32_t alignment = 0; // header +0x0c
    int32_t minIndex = 0, maxIndex = -1;
};

struct Field {
    int32_t index = 0;        // -3 __guid, -2 __name, -1 __base, 0.. declared fields
    std::string name;         // "" for a field without one
    uint64_t typeLayout = 0;  // the field's own type (its layout; 0 when not set)
    std::string typeName;     // that type's name
    uint32_t typeId = 0;      // record +0x00: the field type's id
    uint32_t elementSize = 0; // record +0x04 (0x10 for a reference field, flags & 1)
    uint32_t offset = 0;      // record +0x08: where it is in the type's data
    uint16_t count = 0;       // record +0x10: array length (1: a single value)
    uint16_t flags = 0;       // record +0x12: 1 a reference (16 bytes), 2 / 4 / 8 alignment at least 4 / 8 / 16
};

// Header and record checks keep a bad pointer from reading far: index ranges and table sizes are
// bounded.
bool readType(const reflect::MemoryView& m, uint64_t layout, Type& out);
// Every record of the type, __guid / __name / __base included, by index.
bool readFields(const reflect::MemoryView& m, const Type& t, std::vector<Field>& out);

// The registry (the object g_antTypeRegistry 0x1423bd9b0 points at): a sorted vector of 16-byte
// entries {u32 name hash, u32 pad, Schema* schema}, begin at +0x00 and end at +0x08. A Schema is
// {layout, factory}: the factory is the address of a pointer to the type's function table, whose
// +0x00 makes an object and +0x08 loads one from ANT's data (field by field: research/ant.md).
struct Entry {
    uint32_t hash = 0;
    uint64_t schema = 0, layout = 0, functions = 0; // functions: the table (0 when not readable)
};
std::vector<Entry> registry(const reflect::MemoryView& m, uint64_t registryObject, size_t max = 8192);

// One type's layout by name, as ant_TypeRegistry_getTypeByName (0x142c2d4b0) finds it; 0 if none.
uint64_t findLayout(const reflect::MemoryView& m, uint64_t registryObject, const char* name);

// A field by name, looking in the type, then its base (__base) and so on up; offsets are added up
// along the way (a base's fields sit at the __base field's offset).
bool findField(const reflect::MemoryView& m, uint64_t layout, const char* name, Field& out, int depth = 0);

} // namespace gn::ant
