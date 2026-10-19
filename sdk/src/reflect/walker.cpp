#include "gridnode/reflect/walker.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "gridnode/reflect/layout.hpp"

namespace gn::reflect {

namespace L = gn::layout;

namespace {
constexpr uint64_t kMinValidAddr = 0x10000;
constexpr uint16_t kMaxFields = 2048;
constexpr size_t kScanChunk = 1u << 20;
} // namespace

std::string MemoryView::readCString(uint64_t addr, size_t maxLen) const {
    std::string out;
    if (addr < kMinValidAddr) return out;
    char buf[64];
    while (out.size() < maxLen) {
        const uint64_t at = addr + out.size();
        if (read(at, buf, sizeof(buf))) {
            for (char c : buf) {
                if (c == 0) return out;
                if (c < 0x20 || c > 0x7e) return {};
                out.push_back(c);
                if (out.size() >= maxLen) return {};
            }
            continue;
        }
        // Near the end of a readable range: go byte by byte.
        char c = 0;
        if (!read(at, &c, 1)) return {};
        if (c == 0) return out;
        if (c < 0x20 || c > 0x7e) return {};
        out.push_back(c);
    }
    return {};
}

const char* categoryName(Category c) {
    switch (c) {
    case Category::Primitive: return "Primitive";
    case Category::ValueType: return "ValueType";
    case Category::Class: return "Class";
    case Category::Array: return "Array";
    case Category::FixedArray: return "FixedArray";
    case Category::Enum: return "Enum";
    case Category::Function: return "Function";
    case Category::BoxedValueRef: return "BoxedValueRef";
    case Category::Interface: return "Interface";
    case Category::Delegate: return "Delegate";
    case Category::Unknown: break;
    }
    return "Unknown";
}

const char* primitiveName(uint32_t code) {
    switch (code) {
    case 0: return "Void";
    case 1: return "DbObject";
    case 6: return "String";
    case 7: return "CString";
    case 9: return "FileRef";
    case 10: return "Boolean";
    case 11: return "Int8";
    case 12: return "Uint8";
    case 13: return "Int16";
    case 14: return "Uint16";
    case 15: return "Int32";
    case 16: return "Uint32";
    case 17: return "Int64";
    case 18: return "Uint64";
    case 19: return "Float32";
    case 20: return "Float64";
    case 21: return "Guid";
    case 22: return "SHA1";
    case 23: return "ResourceRef";
    case 25: return "TypeRef";
    default: return nullptr;
    }
}

Category categoryFromFlags(uint16_t flags) {
    switch (L::typeCode(flags)) {
    case 2: return Category::ValueType;
    case 3: return Category::Class;
    case 4: return Category::Array;
    case 5: return Category::FixedArray;
    case 8: return Category::Enum;
    case 24: return Category::Function;
    case 26: return Category::BoxedValueRef;
    case 27: return Category::Interface;
    case 28: return Category::Delegate;
    default: return primitiveName(L::typeCode(flags)) ? Category::Primitive : Category::Unknown;
    }
}

const Type* Reflected::find(uint64_t ti) const {
    auto it = byTi.find(ti);
    return it == byTi.end() ? nullptr : &types[it->second];
}

const Type* Reflected::findByName(const std::string& name) const {
    for (const Type& t : types)
        if (t.name == name) return &t;
    return nullptr;
}

std::string Reflected::displayName(uint64_t ti) const {
    std::string prefix, suffix;
    for (int depth = 0; depth < 8; ++depth) {
        const Type* t = find(ti);
        if (!t) return prefix + (ti ? "?" : "void") + suffix;
        const bool arr = t->category == Category::Array || t->category == Category::FixedArray;
        if (!arr || !t->elementTi || t->elementTi == ti || (t->category == Category::FixedArray && !t->name.empty()))
            return prefix + t->name + suffix;
        prefix += t->category == Category::Array ? "Array<" : "FixedArray<";
        suffix += ">";
        ti = t->elementTi;
    }
    return prefix + "?" + suffix;
}

namespace {
// Bytes from a ClassInfo to the next known record (some ClassInfos have no +0x50/+0x58 slots).
uint64_t classInfoRoom(const Reflected& r, uint64_t ti) {
    auto next = std::upper_bound(r.recordStarts.begin(), r.recordStarts.end(), ti);
    return next == r.recordStarts.end() ? UINT64_MAX : *next - ti;
}
} // namespace

size_t countTypes(const MemoryView& mem, uint64_t listHead, size_t maxTypes) {
    uint64_t ti = mem.readOr<uint64_t>(listHead);
    size_t n = 0;
    while (ti >= kMinValidAddr && n < maxTypes) {
        uint64_t next = 0;
        if (!mem.read(ti + L::TI_Next, next)) break;
        ++n;
        ti = next;
    }
    return n;
}

namespace {

class Walker {
public:
    Walker(const MemoryView& mem, const WalkOptions& opt) : mem_(mem), opt_(opt) {}

    Reflected run() {
        collectList();
        for (size_t i = 0; i < out_.types.size(); ++i) readType(out_.types[i]);
        addReferencedTypes();
        index();
        if (!opt_.regions.empty()) {
            scanAccessors();
            classInfoSlots();
            scanDataForVtables();
        }
        return std::move(out_);
    }

private:
    const MemoryView& mem_;
    const WalkOptions& opt_;
    Reflected out_;
    std::unordered_set<uint64_t> known_;
    std::unordered_map<uint64_t, std::string> modules_;
    std::unordered_map<uint64_t, uint64_t> accessorOwner_; // accessor fn -> ti

    bool inCode(uint64_t a) const {
        for (const Region& r : opt_.regions)
            if (r.code && a >= r.begin && a < r.end) return true;
        return false;
    }

    bool looksLikeTypeInfo(uint64_t ti) const {
        if (ti < kMinValidAddr) return false;
        uint64_t data = 0, namePtr = 0;
        if (!mem_.read(ti, data) || data < kMinValidAddr || !mem_.read(data, namePtr)) return false;
        uint8_t probe;
        return namePtr == 0 || mem_.read(namePtr, &probe, 1);
    }

    void collectList() {
        uint64_t ti = mem_.readOr<uint64_t>(opt_.listHead);
        while (ti >= kMinValidAddr && out_.types.size() < opt_.maxTypes && !known_.count(ti)) {
            uint64_t next = 0;
            if (!mem_.read(ti + L::TI_Next, next)) {
                ++out_.stats.unreadable;
                break;
            }
            known_.insert(ti);
            Type t;
            t.ti = ti;
            out_.types.push_back(std::move(t));
            ti = next;
        }
        out_.stats.listed = out_.types.size();
    }

    const std::string& moduleName(uint64_t mod) {
        auto it = modules_.find(mod);
        if (it != modules_.end()) return it->second;
        std::string n = mod >= kMinValidAddr ? mem_.readCString(mem_.readOr<uint64_t>(mod), 64) : std::string();
        return modules_.emplace(mod, std::move(n)).first->second;
    }

    uint64_t knownOrZero(uint64_t ti, uint64_t self) const { return ti != self && known_.count(ti) ? ti : 0; }

    void readType(Type& t) {
        t.data = mem_.readOr<uint64_t>(t.ti + L::TI_Data);
        t.runtimeId = mem_.readOr<uint16_t>(t.ti + L::TI_RuntimeId);
        if (t.data < kMinValidAddr) {
            ++out_.stats.unknownCategory;
            return;
        }
        t.name = mem_.readCString(mem_.readOr<uint64_t>(t.data + L::TID_Name));
        t.flags = mem_.readOr<uint16_t>(t.data + L::TID_Flags);
        t.size = mem_.readOr<uint16_t>(t.data + L::TID_Size);
        t.alignment = mem_.readOr<uint8_t>(t.data + L::TID_Alignment);
        t.module = moduleName(mem_.readOr<uint64_t>(t.data + L::TID_Module));
        t.category = categoryFromFlags(t.flags);
        if (t.category == Category::Unknown) ++out_.stats.unknownCategory;

        uint32_t fieldsOff = 0;
        switch (t.category) {
        case Category::Class: {
            uint64_t sup = knownOrZero(mem_.readOr<uint64_t>(t.ti + L::CI_Super), t.ti);
            if (!sup) sup = knownOrZero(mem_.readOr<uint64_t>(t.data + L::TID_ClassSuper), t.ti);
            t.superTi = sup;
            t.defaultInstance = mem_.readOr<uint64_t>(t.ti + L::CI_DefaultInstance);
            t.classId = mem_.readOr<uint16_t>(t.ti + L::CI_ClassId);
            t.lastSubClassId = mem_.readOr<uint16_t>(t.ti + L::CI_LastSubClassId);
            t.createFn = mem_.readOr<uint64_t>(t.data + L::TID_ClassCreate);
            fieldsOff = L::TID_ClassFields;
            break;
        }
        case Category::ValueType:
            t.staticInstance = mem_.readOr<uint64_t>(t.data + L::TID_ValueTypeInstance);
            fieldsOff = L::TID_ValueTypeFields;
            break;
        case Category::Enum: fieldsOff = L::TID_EnumFields; break;
        case Category::Array:
        case Category::FixedArray: t.elementTi = mem_.readOr<uint64_t>(t.data + L::TID_ArrayElement); break;
        default: break;
        }
        if (!fieldsOff) return;
        const uint16_t count = mem_.readOr<uint16_t>(t.data + L::TID_FieldCount);
        const uint64_t p = mem_.readOr<uint64_t>(t.data + fieldsOff);
        if (!count || count > kMaxFields || p < kMinValidAddr) return;
        for (uint16_t i = 0; i < count; ++i) {
            const uint64_t e = p + uint64_t(i) * L::FID_Stride;
            uint8_t raw[L::FID_Stride];
            if (!mem_.read(e, raw, sizeof(raw))) break;
            uint64_t namePtr, type;
            uint16_t flags, offset;
            memcpy(&namePtr, raw + L::FID_Name, 8);
            memcpy(&flags, raw + L::FID_Flags, 2);
            memcpy(&offset, raw + L::FID_Offset, 2);
            memcpy(&type, raw + L::FID_Type, 8);
            std::string name = mem_.readCString(namePtr);
            if (name.empty()) name = "field_" + std::to_string(i);
            if (t.category == Category::Enum) {
                int32_t v;
                memcpy(&v, raw + L::FID_Type, 4);
                t.values.push_back({std::move(name), v});
            } else {
                t.fields.push_back({std::move(name), flags, offset, type});
            }
        }
    }

    // Types referenced as super / element / field type but missing from the list.
    void addReferencedTypes() {
        for (int round = 0; round < 8; ++round) {
            std::set<uint64_t> refs;
            for (const Type& t : out_.types) {
                refs.insert(t.superTi);
                refs.insert(t.elementTi);
                for (const Field& f : t.fields) refs.insert(f.typeTi);
            }
            size_t before = out_.types.size();
            for (uint64_t r : refs) {
                if (r < kMinValidAddr || known_.count(r) || !looksLikeTypeInfo(r)) continue;
                known_.insert(r);
                Type t;
                t.ti = r;
                out_.types.push_back(std::move(t));
                readType(out_.types.back());
            }
            if (out_.types.size() == before) break;
        }
        // Array element pointers were read before every type was known: keep only real types.
        for (Type& t : out_.types)
            if (t.elementTi && (!known_.count(t.elementTi) || t.elementTi == t.ti)) t.elementTi = 0;
    }

    void index() {
        std::sort(out_.types.begin(), out_.types.end(), [](const Type& a, const Type& b) { return a.ti < b.ti; });
        out_.byTi.clear();
        for (size_t i = 0; i < out_.types.size(); ++i) out_.byTi[out_.types[i].ti] = i;
    }

    Type* type(uint64_t ti) {
        auto it = out_.byTi.find(ti);
        return it == out_.byTi.end() ? nullptr : &out_.types[it->second];
    }

    // getType accessors: `lea rax,[rip+ti]; ret` (48 8D 05 disp32 C3), or `mov rax,[rip+p]; ret`
    // (48 8B 05 disp32 C3) loading a TypeInfo pointer (messages). Found by byte pattern over code,
    // independent of function boundaries.
    void scanAccessors() {
        for (const Region& r : opt_.regions) {
            if (!r.code) continue;
            forEachBlock(r, 8, [&](const uint8_t* buf, uint64_t base, size_t len) {
                for (size_t i = 0; i + 8 <= len; ++i) {
                    // lea rax,[rip+ti] / mov rax,[rip+&ti]; ret
                    if (buf[i] != 0x48 || (buf[i + 1] != 0x8D && buf[i + 1] != 0x8B) || buf[i + 2] != 0x05 ||
                        buf[i + 7] != 0xC3)
                        continue;
                    int32_t disp;
                    memcpy(&disp, &buf[i + 3], 4);
                    const uint64_t fn = base + i;
                    const uint64_t ea = fn + 7 + int64_t(disp);
                    const uint64_t target = buf[i + 1] == 0x8D ? ea : mem_.readOr<uint64_t>(ea);
                    Type* t = type(target);
                    if (!t || accessorOwner_.count(fn)) continue;
                    accessorOwner_[fn] = target;
                    t->typeAccessors.push_back(fn);
                    ++out_.stats.accessors;
                }
            });
        }
    }

    // Calls fn(buf, base, len) over [r.begin, r.end) in 1 MB blocks extended by `overlap` bytes,
    // so a pattern that crosses a block edge is still seen (callers drop the duplicate match).
    // A block that cannot be read is retried page by page.
    template <class Fn> void forEachBlock(const Region& r, size_t overlap, Fn fn) {
        constexpr size_t kPage = 0x1000;
        std::vector<uint8_t> buf(kScanChunk + overlap);
        for (uint64_t a = r.begin; a < r.end; a += kScanChunk) {
            const size_t len = size_t(std::min<uint64_t>(kScanChunk + overlap, r.end - a));
            if (mem_.read(a, buf.data(), len)) {
                fn(buf.data(), a, len);
                continue;
            }
            const uint64_t blockEnd = std::min<uint64_t>(a + kScanChunk, r.end);
            for (uint64_t p = a; p < blockEnd; p += kPage) {
                const size_t plen = size_t(std::min<uint64_t>(kPage, blockEnd - p));
                if (mem_.read(p, buf.data(), plen)) fn(buf.data(), p, plen);
                else ++out_.stats.unreadablePages;
            }
        }
    }

    // ClassInfo+0x50/+0x58: a data class's vtable, or a runtime class's creators.
    void classInfoSlots() {
        // Record starts bound how long a ClassInfo can be.
        out_.recordStarts.clear();
        for (const Type& t : out_.types) {
            out_.recordStarts.push_back(t.ti);
            if (t.data) out_.recordStarts.push_back(t.data);
        }
        std::sort(out_.recordStarts.begin(), out_.recordStarts.end());
        out_.recordStarts.erase(std::unique(out_.recordStarts.begin(), out_.recordStarts.end()), out_.recordStarts.end());
        for (Type& t : out_.types) {
            // Messages: value types whose static instance (TypeInfoData+0x20) starts with their vtable.
            if (t.category == Category::ValueType) {
                const uint64_t vt = t.staticInstance >= kMinValidAddr ? mem_.readOr<uint64_t>(t.staticInstance) : 0;
                const uint64_t fn = vt >= kMinValidAddr ? mem_.readOr<uint64_t>(vt + 8ull * L::kGetTypeSlot) : 0;
                auto it = accessorOwner_.find(fn);
                if (it != accessorOwner_.end() && it->second == t.ti) {
                    t.vtables.assign(1, vt);
                    t.vtableConfirmed = true;
                    t.getType = fn;
                    ++out_.stats.vtablesFromStaticInstance;
                } else {
                    t.staticInstance = 0; // only kept when it proves to be a message instance
                }
                continue;
            }
            if (t.category != Category::Class) continue;
            if (classInfoRoom(out_, t.ti) < L::CI_SizeShort + 8) {
                ++out_.stats.shortClassInfos;
                continue;
            }
            // A data class's ClassInfo+0x50 holds its vtable (getType at slot 0).
            const uint64_t p = mem_.readOr<uint64_t>(t.ti + L::CI_Slot50);
            if (p >= kMinValidAddr) {
                const uint64_t fn = mem_.readOr<uint64_t>(p + 8ull * L::kGetTypeSlot);
                auto it = accessorOwner_.find(fn);
                if (it != accessorOwner_.end() && it->second == t.ti) {
                    t.vtables.insert(t.vtables.begin(), p);
                    t.vtableConfirmed = true;
                    t.getType = fn;
                    ++out_.stats.vtablesFromClassInfo;
                    continue;
                }
            }
            t.creators = readCreators(mem_, out_, opt_.regions, t);
            out_.stats.creators += t.creators.size();
        }
    }

    // Classes with a getType accessor but no vtable from their ClassInfo: any pointer to the
    // accessor in image data at the getType slot is a vtable of that class (or of a subclass with
    // no reflection of its own, hence possibly several candidates).
    void scanDataForVtables() {
        std::unordered_map<uint64_t, std::vector<uint64_t>> found; // ti -> vtables
        // Every region, code included: page protections do not reliably separate vtables from
        // code in this image (the Ghidra dumper found the same with the Scylla dump's sections).
        for (const Region& r : opt_.regions) {
            const Region aligned{(r.begin + 7) & ~7ull, r.end & ~7ull, false};
            if (aligned.begin >= aligned.end) continue;
            forEachBlock(aligned, 0, [&](const uint8_t* buf, uint64_t base, size_t len) {
                for (size_t i = 0; i + 8 <= len; i += 8) {
                    uint64_t v;
                    memcpy(&v, &buf[i], 8);
                    auto it = accessorOwner_.find(v);
                    if (it == accessorOwner_.end()) continue;
                    found[it->second].push_back(base + i - 8ull * L::kGetTypeSlot);
                }
            });
        }
        for (auto& [ti, vts] : found) {
            Type* t = type(ti);
            // Value types only have a vtable when they are messages (confirmed from their static
            // instance above); other pointers to their accessors are type tables, not vtables.
            if (!t || t->vtableConfirmed || t->category != Category::Class) continue;
            std::sort(vts.begin(), vts.end());
            vts.erase(std::unique(vts.begin(), vts.end()), vts.end());
            t->vtables = vts;
            // Every candidate holds this type's accessor at the getType slot.
            t->getType = mem_.readOr<uint64_t>(vts[0] + 8ull * L::kGetTypeSlot);
            if (vts.size() == 1) ++out_.stats.vtablesFromDataScan;
            else ++out_.stats.vtableCandidatesAmbiguous;
        }
    }
};

} // namespace

Reflected walk(const MemoryView& mem, const WalkOptions& opt) { return Walker(mem, opt).run(); }

std::vector<Creator> readCreators(const MemoryView& mem, const Reflected& r, const std::vector<Region>& regions,
                                  const Type& t) {
    std::vector<Creator> out;
    if (t.category != Category::Class) return out;
    const uint64_t room = classInfoRoom(r, t.ti);
    if (room < L::CI_SizeShort + 8) return out;
    auto inCode = [&](uint64_t a) {
        for (const Region& g : regions)
            if (g.code && a >= g.begin && a < g.end) return true;
        return false;
    };
    const uint64_t slots[2] = {mem.readOr<uint64_t>(t.ti + L::CI_Slot50),
                               room >= L::CI_SizeLong ? mem.readOr<uint64_t>(t.ti + L::CI_Slot58) : 0};
    for (uint32_t realm = 0; realm < 2; ++realm) {
        const uint64_t p = slots[realm];
        if (p < kMinValidAddr) continue;
        if (realm == 0 && t.vtableConfirmed && !t.vtables.empty() && t.vtables[0] == p) continue; // a data class's vtable
        const uint64_t cvt = mem.readOr<uint64_t>(p + L::Creator_Vtable);
        const uint64_t dataTi = mem.readOr<uint64_t>(p + L::Creator_DataTi);
        const uint32_t creatorRealm = mem.readOr<uint32_t>(p + L::Creator_Realm, 0xffffffffu);
        const Type* data = r.find(dataTi);
        if (cvt < kMinValidAddr || !inCode(mem.readOr<uint64_t>(cvt)) || !data || data == &t ||
            data->category != Category::Class || creatorRealm != realm)
            continue;
        out.push_back({p, realm, dataTi});
    }
    return out;
}

} // namespace gn::reflect
