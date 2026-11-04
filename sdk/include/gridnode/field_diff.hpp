#pragma once
// Field diff: what changed in an object between two snapshots of its bytes, named by its reflected
// fields where they cover the change, else by offset. Portable (tested on the host).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gn::fielddiff {

struct FieldSpan {
    std::string name;
    uint32_t offset = 0;
    uint32_t size = 0;
};

struct Change {
    std::string name; // the field, or "+0x1c" for bytes no field covers
    uint32_t offset = 0;
    uint32_t size = 0;
};

// Every field whose bytes differ, then the changed 4-byte words no field covers (in offset order).
// before and after are the same length.
inline std::vector<Change> diff(const std::vector<uint8_t>& before, const std::vector<uint8_t>& after, const std::vector<FieldSpan>& fields) {
    std::vector<Change> out;
    const size_t n = std::min(before.size(), after.size());
    std::vector<bool> covered(n, false);
    for (const FieldSpan& f : fields) {
        if (f.size == 0 || f.offset >= n) continue;
        const size_t end = std::min<size_t>(n, size_t(f.offset) + f.size);
        for (size_t i = f.offset; i < end; ++i) covered[i] = true;
        if (std::memcmp(before.data() + f.offset, after.data() + f.offset, end - f.offset) != 0)
            out.push_back({f.name, f.offset, static_cast<uint32_t>(end - f.offset)});
    }
    for (size_t at = 0; at < n; at += 4) {
        const size_t end = std::min(n, at + 4);
        bool changed = false, uncovered = false;
        for (size_t i = at; i < end; ++i) {
            if (before[i] != after[i]) changed = true;
            if (!covered[i] && before[i] != after[i]) uncovered = true;
        }
        if (!changed || !uncovered) continue;
        char name[24];
        std::snprintf(name, sizeof(name), "+0x%zx", at);
        out.push_back({name, static_cast<uint32_t>(at), static_cast<uint32_t>(end - at)});
    }
    std::stable_sort(out.begin(), out.end(), [](const Change& a, const Change& b) { return a.offset < b.offset; });
    return out;
}

// Bytes as a reader would want them: 1 byte as a number, 4 as a float when it looks like one (else
// an integer), 8 as hex, 12 / 16 as floats, anything else as hex bytes.
inline std::string format(const uint8_t* p, uint32_t size) {
    char b[160];
    auto asFloat = [](const uint8_t* q) {
        float f;
        std::memcpy(&f, q, 4);
        return f;
    };
    auto plausible = [](float f) { return f == 0.f || (f == f && f > -1e7f && f < 1e7f && (f > 1e-6f || f < -1e-6f)); };
    if (size == 1) {
        std::snprintf(b, sizeof(b), "%u", p[0]);
    } else if (size == 2) {
        uint16_t v;
        std::memcpy(&v, p, 2);
        std::snprintf(b, sizeof(b), "%u", v);
    } else if (size == 4) {
        int32_t i;
        std::memcpy(&i, p, 4);
        const float f = asFloat(p);
        if (plausible(f) && (i < -0x10000 || i > 0x10000)) std::snprintf(b, sizeof(b), "%.6g", f);
        else std::snprintf(b, sizeof(b), "%d", i);
    } else if (size == 8) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(v));
    } else if (size == 12 || size == 16) {
        std::string s;
        for (uint32_t i = 0; i < size; i += 4) {
            std::snprintf(b, sizeof(b), "%s%.4g", i ? " " : "", asFloat(p + i));
            s += b;
        }
        return s;
    } else {
        std::string s;
        for (uint32_t i = 0; i < size && i < 32; ++i) {
            std::snprintf(b, sizeof(b), "%02x", p[i]);
            s += b;
        }
        return size > 32 ? s + "..." : s;
    }
    return b;
}

} // namespace gn::fielddiff
