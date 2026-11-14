// One copy of a static model group's member switched on or off, or hidden, as the level's own logic does it
// (API 41): the group's vtable slot 2 takes a member copy's packed property (ClientStaticModelGroupEntity_
// propertyChanged, ServerStaticModelGroupEntity_propertyChanged in db/annotations/render.yaml; read in Ghidra
// 2026-11-14, from a modder's notes). Enabled false takes the copy's render object and collision out (the client
// group) or its collision (the server group); true makes them again. Visible only shows or hides the render
// object (client). The layout editor uses it instead of parking a group copy 15 km down.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

// Both groups read their data at +0x30, its members at +0x90: an array of 0x68-byte StaticModelGroupMemberData
// (count at -4), as 0x1431840f0 and the server's handler index them.
constexpr uint64_t kData = 0x30, kMembers = 0x90, kMemberSize = 0x68;
constexpr uint32_t kMaxIndex = 0x7ff; // 11 bits each in the packed id

struct Change {
    uint32_t id;
    uint32_t pad;
    const uint8_t* value;
};

uint32_t instanceCountField() {
    static const uint32_t off = [] {
        const reflect::Type* t = registry().find("StaticModelGroupMemberData");
        reflect::FieldInfo fi;
        return t && registry().findField(t, "InstanceCount", fi) ? fi.field->offset : 0xffffffffu;
    }();
    return off;
}

} // namespace

int staticMemberSet(void* group, uint32_t member, uint32_t copy, uint32_t what, int value) {
    static const reflect::Type* client = registry().find("ClientStaticModelGroupEntity");
    static const reflect::Type* server = registry().find("ServerStaticModelGroupEntity");
    static const reflect::Type* dataType = registry().find("StaticModelGroupEntityData");
    const uintptr_t g = reinterpret_cast<uintptr_t>(group);
    const reflect::Type* t = g ? registry().typeOf(mem::ProcessMemory(), g) : nullptr;
    const bool isClient = t && client && registry().isKindOf(t, client);
    const bool isServer = t && server && registry().isKindOf(t, server);
    if ((!isClient && !isServer) || !inspector::alive(g, t)) return GN_ERR_ARGUMENT;
    if (what != GN_MEMBER_ENABLED && !(what == GN_MEMBER_VISIBLE && isClient)) return GN_ERR_ARGUMENT; // the server group only switches
    if (isClient ? !isClientThread() : !isServerThread()) return GN_ERR_THREAD;
    // The game's handlers check neither index (the client's not the member, neither the copy): both here.
    const uintptr_t data = mem::readOr<uintptr_t>(g + kData);
    const reflect::Type* dt = data ? registry().typeOf(mem::ProcessMemory(), data) : nullptr;
    if (!dt || !dataType || !registry().isKindOf(dt, dataType)) return GN_ERR_UNAVAILABLE;
    const uintptr_t members = mem::readOr<uintptr_t>(data + kMembers);
    const uint32_t count = members ? mem::readOr<uint32_t>(members - 4) : 0;
    if (member >= count || member + 1 > kMaxIndex) return GN_ERR_NOT_FOUND;
    const uint32_t icf = instanceCountField();
    if (icf == 0xffffffffu) return GN_ERR_UNAVAILABLE;
    const uint32_t copies = mem::readOr<uint32_t>(members + member * kMemberSize + icf);
    if (copy >= copies || copy > kMaxIndex) return GN_ERR_NOT_FOUND;
    const uint8_t v = value ? 1 : 0;
    const Change c{what | ((member + 1) << 2) | (copy << 13), 0, &v};
    using Fn = void (*)(void*, const Change*);
    const uintptr_t vt = mem::readOr<uintptr_t>(g);
    const uintptr_t fn = vt ? mem::readOr<uintptr_t>(vt + 0x10) : 0;
    if (!fn) return GN_ERR_UNAVAILABLE;
    reinterpret_cast<Fn>(fn)(group, &c);
    return GN_OK;
}

// "member ADDRESS MEMBER COPY on|off|show|hide": one copy of a group (either realm), on its realm thread.
std::string memberCommand(const std::string& rest) {
    if (rest.find("pick") != std::string::npos) return runOnRealmAndWait(true, [] { return layouts::memberPick(); });
    unsigned long long addr = 0;
    unsigned member = 0, copy = 0;
    char word[8] = {};
    if (sscanf(rest.c_str(), " %llx %u %u %7s", &addr, &member, &copy, word) != 4)
        return "member pick (the group prop you face) | member ADDRESS MEMBER COPY on|off|show|hide (a Client or ServerStaticModelGroupEntity)";
    const bool on = !_stricmp(word, "on") || !_stricmp(word, "show");
    const uint32_t what = (!_stricmp(word, "show") || !_stricmp(word, "hide")) ? GN_MEMBER_VISIBLE : GN_MEMBER_ENABLED;
    void* group = reinterpret_cast<void*>(static_cast<uintptr_t>(addr));
    const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), static_cast<uintptr_t>(addr));
    const bool serverRealm = t && t->name.find("Server") != std::string::npos;
    return runOnRealmAndWait(serverRealm, [=]() -> std::string {
        const int r = staticMemberSet(group, member, copy, what, on ? 1 : 0);
        return r == GN_OK ? "member: done" : "member: error " + std::to_string(r);
    });
}

} // namespace gn::host
