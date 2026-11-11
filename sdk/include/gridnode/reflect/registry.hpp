#pragma once
// Runtime type registry: the walked reflection data, indexed for lookups while the game runs.
// Portable (memory goes through MemoryView), so it is tested on a fake image like the walker.

#include <cstdint>
#include <string>
#include <unordered_map>

#include "gridnode/reflect/walker.hpp"

namespace gn::reflect {

struct FieldInfo {
    const Field* field = nullptr;
    const Type* owner = nullptr; // the class / value type that declares the field
    const Type* type = nullptr;  // the field's type (may be null if unknown)
    uint32_t size = 0;           // bytes the field occupies in the object
};

class Registry {
public:
    // Takes the walked data. Builds name, TypeInfo and accessor indexes.
    void build(Reflected r);
    bool empty() const { return r_.types.empty(); }
    const Reflected& data() const { return r_; }

    const Type* find(const std::string& name) const;
    const Type* byTi(uint64_t ti) const { return r_.find(ti); }
    const Type* super(const Type* t) const { return t && t->superTi ? r_.find(t->superTi) : nullptr; }

    // The object's type, from the getType accessor in its vtable slot 0. Calls nothing in the
    // game: the accessor is looked up among the ones the walker found. Null if unknown.
    const Type* typeOf(const MemoryView& mem, uint64_t object) const;
    // The same with plain loads, for an object the engine has just handed over (a hook's argument:
    // a creator's new entity, a destructor's self, an event's peer). typeOf reads through
    // ReadProcessMemory, a kernel call per read, and ran for every entity created and destroyed and
    // every event delivered (review, 2026-11-11). Never for a pointer of unknown origin.
    const Type* typeOfLive(const void* object) const;

    // Class: t is base or derives from it (class id range). Value types and others: equality.
    bool isKindOf(const Type* t, const Type* base) const;

    // Field by name in t and its bases (the most derived declaration wins).
    bool findField(const Type* t, const std::string& name, FieldInfo& out) const;
    FieldInfo describe(const Type* owner, const Field& f) const;
    // Bytes a field of this type occupies: 8 for classes (references) and arrays.
    uint32_t fieldSize(uint64_t typeTi) const;

private:
    Reflected r_;
    std::unordered_map<std::string, const Type*> byName_;
    std::unordered_map<uint64_t, const Type*> byAccessor_;
};

} // namespace gn::reflect
