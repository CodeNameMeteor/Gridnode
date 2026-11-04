#pragma once
// Field diff: what changed in an object between two snapshots of its bytes, named by its reflected
// fields where they cover the change, else by offset. Portable (tested on the host).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
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

// A 64-bit value that looks like a pointer to an object: user space above 4 GB (where the game's heap
// and image are), 8-aligned, low half not zero (an integer 1 in the high word is not one).
inline bool pointerLike(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v >= 0x100000000ull && v < 0x800000000000ull && (v & 7) == 0 && (v & 0xffffffffull) != 0;
}

// Every field whose bytes differ, then the changed bytes no field covers (in offset order): an
// 8-aligned pointer-looking qword as one change, else 4-byte words. before and after are the same length.
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
    for (size_t at = 0, step = 4; at < n; at += step) {
        step = (at % 8 == 0 && at + 8 <= n && (pointerLike(before.data() + at) || pointerLike(after.data() + at))) ? 8 : 4;
        const size_t end = std::min(n, at + step);
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

// Both sides of a change the same way (a word that is a float on one side reads as one on the other).
inline std::pair<std::string, std::string> formatBoth(const uint8_t* before, const uint8_t* after, uint32_t size);

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

inline std::pair<std::string, std::string> formatBoth(const uint8_t* before, const uint8_t* after, uint32_t size) {
    if (size != 4) return {format(before, size), format(after, size)};
    auto floaty = [](const uint8_t* q) {
        int32_t i;
        float f;
        std::memcpy(&i, q, 4), std::memcpy(&f, q, 4);
        const bool plausible = f == 0.f || (f == f && f > -1e7f && f < 1e7f && (f > 1e-6f || f < -1e-6f));
        return std::pair<bool, bool>{plausible, i < -0x10000 || i > 0x10000}; // could be one, looks like one
    };
    const auto [pa, la] = floaty(before);
    const auto [pb, lb] = floaty(after);
    char x[48], y[48];
    if (pa && pb && (la || lb)) {
        float f, g;
        std::memcpy(&f, before, 4), std::memcpy(&g, after, 4);
        std::snprintf(x, sizeof(x), "%.6g", f), std::snprintf(y, sizeof(y), "%.6g", g);
    } else {
        int32_t i, j;
        std::memcpy(&i, before, 4), std::memcpy(&j, after, 4);
        if ((la && !pa) || (lb && !pb)) std::snprintf(x, sizeof(x), "0x%x", uint32_t(i)), std::snprintf(y, sizeof(y), "0x%x", uint32_t(j));
        else std::snprintf(x, sizeof(x), "%d", i), std::snprintf(y, sizeof(y), "%d", j);
    }
    return {x, y};
}

} // namespace gn::fielddiff
