#include "gridnode/reflect/dump_json.hpp"

#include <cinttypes>
#include <cstdio>

namespace gn::reflect {

namespace {

std::string hex(uint64_t v) {
    char b[24];
    snprintf(b, sizeof(b), "0x%" PRIx64, v);
    return b;
}

std::string quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", c);
                o += b;
            } else {
                o += char(c);
            }
        }
    }
    return o + "\"";
}

class Va {
public:
    explicit Va(const ImageInfo& i) : i_(i) {}
    bool inImage(uint64_t a) const { return a >= i_.runtimeBase && a < i_.runtimeBase + i_.size; }
    // Image VA as a JSON string, or "" if the address is outside the image (caller omits the key).
    std::string str(uint64_t a) const { return inImage(a) ? quote(hex(a - i_.runtimeBase + i_.preferredBase)) : std::string(); }

private:
    const ImageInfo& i_;
};

void key(std::ostream& o, const char* k, const std::string& jsonValue) {
    if (!jsonValue.empty()) o << ",\"" << k << "\":" << jsonValue;
}

} // namespace

void writeJson(std::ostream& o, const Reflected& r, const DumpInfo& info) {
    const Va va(info.image);
    const WalkStats& s = r.stats;
    o << "{\n";
    o << "\"format\":\"gridnode-reflected\",\"version\":1,\n";
    o << "\"generator\":" << quote(info.generator) << ",\"created\":" << quote(info.createdUtc) << ",\n";
    o << "\"image\":{\"name\":" << quote(info.image.name) << ",\"base\":" << quote(hex(info.image.preferredBase))
      << ",\"size\":" << quote(hex(info.image.size)) << ",\"timestamp\":" << quote(hex(info.image.timestamp)) << "},\n";
    o << "\"stats\":{\"listed\":" << s.listed << ",\"accessors\":" << s.accessors
      << ",\"vtablesFromClassInfo\":" << s.vtablesFromClassInfo
      << ",\"vtablesFromStaticInstance\":" << s.vtablesFromStaticInstance << ",\"vtablesFromDataScan\":" << s.vtablesFromDataScan
      << ",\"vtableCandidatesAmbiguous\":" << s.vtableCandidatesAmbiguous << ",\"creators\":" << s.creators
      << ",\"shortClassInfos\":" << s.shortClassInfos << ",\"unknownCategory\":" << s.unknownCategory
      << ",\"unreadable\":" << s.unreadable << "},\n";
    o << "\"typeCount\":" << r.types.size() << ",\n\"types\":[\n";
    bool first = true;
    for (const Type& t : r.types) {
        if (!first) o << ",\n";
        first = false;
        {
            const std::string ti = va.str(t.ti);
            o << "{\"ti\":" << (ti.empty() ? quote(hex(t.ti)) : ti);
        }
        key(o, "data", va.str(t.data));
        o << ",\"name\":" << quote(t.name) << ",\"displayName\":" << quote(r.displayName(t.ti))
          << ",\"module\":" << quote(t.module) << ",\"category\":\"" << categoryName(t.category) << "\""
          << ",\"flags\":" << t.flags << ",\"size\":" << t.size << ",\"alignment\":" << unsigned(t.alignment)
          << ",\"runtimeId\":" << t.runtimeId;
        key(o, "super", va.str(t.superTi));
        key(o, "element", va.str(t.elementTi));
        if (t.category == Category::Class) {
            o << ",\"classId\":" << t.classId << ",\"lastSubClassId\":" << t.lastSubClassId;
            key(o, "create", va.str(t.createFn));
        }
        key(o, "staticInstance", va.str(t.staticInstance));
        if (t.category == Category::Class || t.category == Category::ValueType) {
            key(o, "getType", va.str(t.getType));
            if (!t.typeAccessors.empty()) {
                o << ",\"accessors\":[";
                for (size_t i = 0; i < t.typeAccessors.size(); ++i) o << (i ? "," : "") << va.str(t.typeAccessors[i]);
                o << "]";
            }
            if (!t.vtables.empty()) {
                o << ",\"vtables\":[";
                for (size_t i = 0; i < t.vtables.size(); ++i) o << (i ? "," : "") << va.str(t.vtables[i]);
                o << "],\"vtableConfirmed\":" << (t.vtableConfirmed ? "true" : "false");
            }
            if (!t.creators.empty()) {
                o << ",\"creators\":[";
                for (size_t i = 0; i < t.creators.size(); ++i) {
                    const Creator& c = t.creators[i];
                    o << (i ? "," : "") << "{\"creator\":" << va.str(c.creator) << ",\"realm\":" << c.realm
                      << ",\"data\":" << va.str(c.dataTi) << "}";
                }
                o << "]";
            }
        }
        if (!t.fields.empty()) {
            o << ",\"fields\":[";
            for (size_t i = 0; i < t.fields.size(); ++i) {
                const Field& f = t.fields[i];
                o << (i ? "," : "") << "{\"name\":" << quote(f.name) << ",\"offset\":" << f.offset
                  << ",\"flags\":" << f.flags;
                key(o, "type", va.str(f.typeTi));
                o << "}";
            }
            o << "]";
        }
        if (!t.values.empty()) {
            o << ",\"values\":[";
            for (size_t i = 0; i < t.values.size(); ++i)
                o << (i ? "," : "") << "{\"name\":" << quote(t.values[i].name) << ",\"value\":" << t.values[i].value << "}";
            o << "]";
        }
        o << "}";
    }
    o << "\n]\n}\n";
}

} // namespace gn::reflect
