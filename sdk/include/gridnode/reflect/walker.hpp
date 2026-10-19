#pragma once
// Reads Frostbite's reflection data (every TypeInfo on the engine's global list) into plain
// records. Portable: all memory access goes through MemoryView, so the same code runs inside
// the game (gn::ProcessMemory) and in host-side tests against a fake memory image.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gn::reflect {

class MemoryView {
public:
    virtual ~MemoryView() = default;
    // Copies `size` bytes at `addr`; false if any byte is unreadable. Must never fault.
    virtual bool read(uint64_t addr, void* out, size_t size) const = 0;

    template <class T> bool read(uint64_t addr, T& out) const { return read(addr, &out, sizeof(T)); }
    template <class T> T readOr(uint64_t addr, T fallback = T{}) const {
        T v{};
        return read(addr, &v, sizeof(T)) ? v : fallback;
    }
    // NUL-terminated printable ASCII, at most maxLen characters. Empty if none.
    std::string readCString(uint64_t addr, size_t maxLen = 512) const;
};

// A range of the game image. Code ranges are scanned for getType accessors, data ranges for vtables.
struct Region {
    uint64_t begin = 0, end = 0;
    bool code = false;
};

struct Field {
    std::string name;
    uint16_t flags = 0;
    uint16_t offset = 0;
    uint64_t typeTi = 0;
};

struct EnumValue {
    std::string name;
    int32_t value = 0;
};

struct Creator {
    uint64_t creator = 0; // creator object (image data)
    uint32_t realm = 0;   // 0 client, 1 server
    uint64_t dataTi = 0;  // data class it creates from
};

enum class Category { Unknown, Primitive, ValueType, Class, Array, FixedArray, Enum, Function, BoxedValueRef, Interface, Delegate };
const char* categoryName(Category c);
Category categoryFromFlags(uint16_t flags);
const char* primitiveName(uint32_t typeCode); // nullptr if not a primitive code

struct Type {
    uint64_t ti = 0;
    uint64_t data = 0;
    std::string name;   // raw name from TypeInfoData (array names are often empty)
    std::string module;
    Category category = Category::Unknown;
    uint16_t flags = 0;
    uint16_t size = 0;
    uint8_t alignment = 0;
    uint16_t runtimeId = 0;
    uint64_t superTi = 0;   // Class
    uint64_t elementTi = 0; // Array / FixedArray
    std::vector<Field> fields;     // Class / ValueType
    std::vector<EnumValue> values; // Enum
    uint64_t staticInstance = 0; // ValueType messages: static instance, vtable at +0
    // Class only
    uint16_t classId = 0, lastSubClassId = 0;
    uint64_t defaultInstance = 0;
    uint64_t createFn = 0;
    // Class and ValueType (messages)
    std::vector<uint64_t> typeAccessors; // functions that return this TypeInfo
    uint64_t getType = 0;                // the accessor in vtable slot 0
    std::vector<uint64_t> vtables;       // the vtable, or candidates (subclasses sharing getType)
    bool vtableConfirmed = false;        // from ClassInfo+0x50 or the message's static instance
    std::vector<Creator> creators;
};

struct WalkOptions {
    uint64_t listHead = 0;             // address of the TypeInfo* list head
    std::vector<Region> regions;       // image ranges; empty disables accessor/vtable discovery
    size_t maxTypes = 200000;          // cycle / corruption guard
};

struct WalkStats {
    size_t listed = 0;
    size_t accessors = 0;
    size_t vtablesFromClassInfo = 0;
    size_t vtablesFromStaticInstance = 0;
    size_t vtablesFromDataScan = 0;
    size_t vtableCandidatesAmbiguous = 0;
    size_t creators = 0;
    size_t shortClassInfos = 0;
    size_t unknownCategory = 0;
    size_t unreadable = 0;
};

struct Reflected {
    std::vector<Type> types; // sorted by ti
    std::map<uint64_t, size_t> byTi;
    WalkStats stats;

    const Type* find(uint64_t ti) const;
    const Type* findByName(const std::string& name) const; // first match in ti order
    // Display name: the raw name, or Array<Element> / FixedArray<Element> for unnamed arrays.
    std::string displayName(uint64_t ti) const;
};

// Number of entries reachable from the list head (0 if unreadable). Cheap: used to wait for the
// engine to finish registering its types.
size_t countTypes(const MemoryView& mem, uint64_t listHead, size_t maxTypes = 200000);

Reflected walk(const MemoryView& mem, const WalkOptions& opt);

} // namespace gn::reflect
