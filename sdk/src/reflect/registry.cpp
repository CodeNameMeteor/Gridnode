#include "gridnode/reflect/registry.hpp"

#include "gridnode/reflect/layout.hpp"

namespace gn::reflect {

void Registry::build(Reflected r) {
    r_ = std::move(r);
    byName_.clear();
    byAccessor_.clear();
    for (const Type& t : r_.types) {
        const std::string name = t.name.empty() ? r_.displayName(t.ti) : t.name;
        byName_.emplace(name, &t); // first in TypeInfo order wins for the few duplicate names
        for (uint64_t a : t.typeAccessors) byAccessor_.emplace(a, &t);
    }
}

const Type* Registry::find(const std::string& name) const {
    auto it = byName_.find(name);
    return it == byName_.end() ? nullptr : it->second;
}

const Type* Registry::typeOf(const MemoryView& mem, uint64_t object) const {
    if (!object) return nullptr;
    uint64_t vt = 0, fn = 0;
    if (!mem.read(object, vt) || !vt) return nullptr;
    if (!mem.read(vt + 8ull * layout::kGetTypeSlot, fn)) return nullptr;
    auto it = byAccessor_.find(fn);
    return it == byAccessor_.end() ? nullptr : it->second;
}

const Type* Registry::typeOfLive(const void* object) const {
    if (!object) return nullptr;
    const auto vt = *static_cast<const uint64_t* const*>(object);
    if (!vt) return nullptr;
    auto it = byAccessor_.find(vt[layout::kGetTypeSlot]);
    return it == byAccessor_.end() ? nullptr : it->second;
}

bool Registry::isKindOf(const Type* t, const Type* base) const {
    if (!t || !base) return false;
    if (t == base) return true;
    if (t->category != Category::Class || base->category != Category::Class) return false;
    // isKindOf as the engine does it: the class id of t lies in base's [classId, lastSubClassId].
    return base->lastSubClassId >= base->classId && t->classId >= base->classId && t->classId <= base->lastSubClassId;
}

uint32_t Registry::fieldSize(uint64_t typeTi) const {
    const Type* t = r_.find(typeTi);
    if (!t) return 0;
    switch (t->category) {
    case Category::Class:
    case Category::Array: return 8;
    default: return t->size;
    }
}

FieldInfo Registry::describe(const Type* owner, const Field& f) const {
    FieldInfo fi;
    fi.field = &f;
    fi.owner = owner;
    fi.type = r_.find(f.typeTi);
    fi.size = fieldSize(f.typeTi);
    return fi;
}

bool Registry::findField(const Type* t, const std::string& name, FieldInfo& out) const {
    for (int depth = 0; t && depth < 64; ++depth, t = super(t)) {
        for (const Field& f : t->fields) {
            if (f.name == name) {
                out = describe(t, f);
                return true;
            }
        }
    }
    return false;
}

} // namespace gn::reflect
