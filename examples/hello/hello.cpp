// hello: the M3 example mod. Put hello.dll in the game's mods/ folder (next to gridnode.dll).
//
// It shows the four things GridNode gives a mod:
//   - reflection: look up an engine type and read fields by name
//   - entity creation: be told about every PamClientCharacterEntity the game creates
//   - ticks: run code on the client and server game threads
//   - engine addresses: call a game function from gridnode/engine.hpp
//   - messages: be told when the engine sends a message
// Everything it does is read-only. Output goes to gridnode.log, prefixed [hello].

#include <cmath>

#include "gridnode/engine.hpp"
#include "gridnode/mod.hpp"

namespace e = gn::engine;

namespace {

struct Vec3 { float x, y, z; };

// A spatial entity's position, through its transform space (see db/annotations/entity.yaml).
bool position(void* entity, Vec3& out) {
    const char* p = static_cast<const char*>(entity);
    const char* ts = nullptr;
    uint8_t sel[2] = {};
    if (!gn::read(p + e::cls::ComponentEntity::off::transformSpace, ts) || !ts || !gn::read(ts + 9, sel)) return false;
    const char* transform = ts + 0x10 + (sel[0] + 2 * sel[1]) * 0x20;
    float trans[4];
    if (!gn::read(transform + 0x30, trans)) return false; // LinearTransform.trans
    out = {trans[0], trans[1], trans[2]};
    return true;
}

void describeType(const char* name) {
    gn::Type t = gn::Type::find(name);
    if (!t) {
        gn::log("type %s: not found", name);
        return;
    }
    std::string chain;
    for (gn::Type s = t.super(); s; s = s.super()) chain += std::string(" : ") + s.name();
    gn::log("type %s (size 0x%x, TypeInfo 0x%llx)%s", t.name(), t.size(), static_cast<unsigned long long>(t.va()),
            chain.c_str());
}

} // namespace

GN_MOD("hello") {
    gn::log("loaded into GridNode %s", gn::api().gridnode_version);
    describeType("PamClientCharacterEntity");
    describeType("PamCharacterEntityData");

    // Every client character the game creates (Faith, AI, runner ghosts): its data asset and two
    // of the data's reflected fields, read by name.
    gn::onCreated("PamClientCharacterEntity", [](void* entity, void*) {
        void* data = nullptr;
        gn::read(static_cast<char*>(entity) + e::cls::ComponentEntity::off::data, data);
        gn::Object d = gn::Object::of(data);
        bool visible = false, ghostShader = false;
        d.get("Visible", visible);
        d.get("PamUseRunnersGhostShader", ghostShader);
        gn::log("client character created: %p (%s), data %p (%s): Visible=%d PamUseRunnersGhostShader=%d", entity,
                gn::Type::of(entity).name(), data, d.type().name(), visible, ghostShader);
    });

    // Every ~2 s on the client thread: where Faith is.
    gn::onClientTick([](float dt) {
        static float t = 0;
        if ((t += dt) < 2.0f) return;
        t = 0;
        auto localCharacter = gn::function<void* (*)()>(e::function::LocalClientCharacter);
        void* ch = localCharacter();
        Vec3 p{};
        if (ch && position(ch, p)) gn::log("client: Faith at (%.2f, %.2f, %.2f)", p.x, p.y, p.z);
        else gn::log("client: no local character");
    });

    // Engine messages: everything in the ServerPlayer category (spawns, deaths, ...: the first 30),
    // and the client being told which character the local player has.
    gn::onMessage("ServerPlayer", [](const void*, const gn_message_info& info) {
        static int n = 0;
        if (++n <= 30) gn::log("message: %s (%s manager)", info.type_name, info.manager == 2 ? "server" : info.manager == 1 ? "client" : "core");
    });
    gn::onMessage("ClientCharacterLocalPlayerSetMessage", [](const void* m, const gn_message_info& info) {
        void* character = nullptr;
        gn::read(static_cast<const char*>(m) + 0x30, character); // the message's member after the header
        gn::log("message: %s, character %p (%s)", info.type_name, character, gn::Type::of(character).name());
    });

    // Every ~5 s on the server thread: how many server ticks ran.
    gn::onServerTick([](float dt) {
        static float t = 0;
        static unsigned ticks = 0;
        ++ticks;
        if ((t += dt) < 5.0f) return;
        gn::log("server: %u ticks in the last %.1f s (on the server thread: %s)", ticks, t,
                gn::isServerThread() ? "yes" : "no");
        t = 0;
        ticks = 0;
    });
    return true;
}
