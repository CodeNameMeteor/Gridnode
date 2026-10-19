#pragma once
// Writes a walked type list as gridnode-reflected JSON (format described in docs/dump-format.md).

#include <cstdint>
#include <ostream>
#include <string>

#include "gridnode/reflect/walker.hpp"

namespace gn::reflect {

struct ImageInfo {
    std::string name;            // e.g. MirrorsEdgeCatalyst.exe
    uint64_t runtimeBase = 0;    // where the image is loaded now
    uint64_t preferredBase = 0;  // image VA base (0x140000000)
    uint64_t size = 0;           // SizeOfImage
    uint32_t timestamp = 0;      // PE TimeDateStamp
};

struct DumpInfo {
    ImageInfo image;
    std::string generator; // "gridnode <version>"
    std::string createdUtc; // ISO 8601, may be empty
};

// Addresses inside the image are written as image VAs (preferred base); addresses outside it
// (heap objects such as default instances) are left out, because they change every session.
void writeJson(std::ostream& out, const Reflected& r, const DumpInfo& info);

} // namespace gn::reflect
