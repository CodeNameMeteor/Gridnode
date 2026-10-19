// gn_offline_dump: runs the GridNode reflection walker on a memory dump of the game saved as a
// PE file (for example a Scylla dump), instead of inside the running game.
//
//   gn_offline_dump <dump.exe> <out.json>
//
// Developer tool: it lets the walker be tested against real game data on any OS. The in-game
// dump (gridnode.dll) remains the way users generate their reflected data.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "gridnode/reflect/dump_json.hpp"
#include "gridnode/reflect/layout.hpp"
#include "gridnode/reflect/walker.hpp"

using namespace gn::reflect;

namespace {

struct Section {
    uint64_t va = 0;   // absolute
    uint64_t size = 0; // virtual size
    std::vector<uint8_t> bytes;
    bool code = false;
};

class PeImage final : public MemoryView {
public:
    std::vector<Section> sections;
    uint64_t imageBase = 0, imageSize = 0;
    uint32_t timestamp = 0;

    bool read(uint64_t addr, void* out, size_t size) const override {
        auto* dst = static_cast<uint8_t*>(out);
        while (size) {
            const Section* s = find(addr);
            if (!s) return false;
            const uint64_t off = addr - s->va;
            const size_t n = size_t(std::min<uint64_t>(size, s->size - off));
            for (size_t i = 0; i < n; ++i) dst[i] = off + i < s->bytes.size() ? s->bytes[off + i] : 0;
            dst += n;
            addr += n;
            size -= n;
        }
        return true;
    }

private:
    const Section* find(uint64_t a) const {
        for (const Section& s : sections)
            if (a >= s.va && a < s.va + s.size) return &s;
        return nullptr;
    }
};

template <class T> T at(const std::vector<uint8_t>& f, size_t off) {
    T v{};
    if (off + sizeof(T) <= f.size()) memcpy(&v, &f[off], sizeof(T));
    return v;
}

bool load(const std::string& path, PeImage& img) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (f.size() < 0x200 || f[0] != 'M' || f[1] != 'Z') return false;
    const uint32_t pe = at<uint32_t>(f, 0x3c);
    if (at<uint32_t>(f, pe) != 0x4550) return false; // "PE\0\0"
    const uint16_t nsec = at<uint16_t>(f, pe + 6);
    img.timestamp = at<uint32_t>(f, pe + 8);
    const uint16_t optSize = at<uint16_t>(f, pe + 20);
    const size_t opt = pe + 24;
    if (at<uint16_t>(f, opt) != 0x20b) return false; // PE32+
    img.imageBase = at<uint64_t>(f, opt + 24);
    img.imageSize = at<uint32_t>(f, opt + 56);
    const size_t secTable = opt + optSize;
    for (uint16_t i = 0; i < nsec; ++i) {
        const size_t h = secTable + size_t(i) * 40;
        Section s;
        const uint32_t vsize = at<uint32_t>(f, h + 8), rva = at<uint32_t>(f, h + 12);
        const uint32_t rawSize = at<uint32_t>(f, h + 16), rawPtr = at<uint32_t>(f, h + 20);
        const uint32_t ch = at<uint32_t>(f, h + 36);
        s.va = img.imageBase + rva;
        s.size = std::max(vsize, rawSize);
        s.code = (ch & 0x20000000u) != 0; // IMAGE_SCN_MEM_EXECUTE
        if (rawPtr < f.size()) {
            const size_t n = std::min<size_t>(rawSize, f.size() - rawPtr);
            s.bytes.assign(f.begin() + rawPtr, f.begin() + rawPtr + n);
        }
        img.sections.push_back(std::move(s));
    }
    // Headers (first page) as data.
    Section hdr;
    hdr.va = img.imageBase;
    hdr.size = 0x1000;
    hdr.bytes.assign(f.begin(), f.begin() + std::min<size_t>(0x1000, f.size()));
    img.sections.push_back(std::move(hdr));
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: gn_offline_dump <dump.exe> <out.json>\n";
        return 2;
    }
    PeImage img;
    if (!load(argv[1], img)) {
        std::cerr << "not a 64-bit PE file: " << argv[1] << "\n";
        return 1;
    }
    WalkOptions opt;
    opt.listHead = gn::layout::kTypeInfoListHead - gn::layout::kImageBase + img.imageBase;
    for (const auto& s : img.sections) opt.regions.push_back({s.va, s.va + s.size, s.code});
    const Reflected r = walk(img, opt);
    const WalkStats& st = r.stats;
    std::cerr << r.types.size() << " types (" << st.listed << " listed), " << st.accessors << " accessors, vtables "
              << st.vtablesFromClassInfo << " from ClassInfo + " << st.vtablesFromStaticInstance << " from messages + " << st.vtablesFromDataScan << " from data scan ("
              << st.vtableCandidatesAmbiguous << " ambiguous), " << st.creators << " creators, " << st.shortClassInfos
              << " short ClassInfos, " << st.unknownCategory << " unknown category\n";

    DumpInfo info;
    std::string name = argv[1];
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    info.image = {name, img.imageBase, gn::layout::kImageBase, img.imageSize, img.timestamp};
    info.generator = "gn_offline_dump";
    std::ofstream out(argv[2], std::ios::binary);
    writeJson(out, r, info);
    return out.good() ? 0 : 1;
}
