#pragma once
// Internal state of the GridNode runtime inside the game (not part of the public API).

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "gridnode/api.h"
#include "gridnode/dispatch.hpp"
#include "gridnode/hook_table.hpp"
#include "gridnode/reflect/registry.hpp"

namespace gn::host {

reflect::Registry& registry();
hook::HookTable& hooks();
thread::Dispatcher& server();
thread::Dispatcher& client();

// Ticks (ticks.cpp): installs the server and client world postUpdate hooks.
struct TickStatus {
    bool server = false;
    bool client = false;
};
TickStatus installTicks();
bool isServerThread();
bool isClientThread();

// Entities (entities.cpp): onCreated through the classes' creator slots. Creators are bound by the
// engine when a realm starts, possibly after GridNode: refreshCreators attaches listeners to
// creators that appeared since (called about once a second).
int onCreated(const std::string& owner, const std::string& className, gn_created_fn fn, void* user);
void refreshCreators();
size_t boundCreatorCount();

// The API table handed to mods (api.cpp).
const gn_api* api();

// Mods (mods.cpp): loads every DLL in `folder` that exports gn_mod_load.
void loadMods(const std::wstring& folder);
struct ModInfo {
    std::string file;
    bool loaded = false;
    int result = 0;
};
std::vector<ModInfo> modList();

// World (world.cpp): characters, transforms, teleport, components, levels.
void worldStart();      // after the registry is built: tracks server characters
void worldClientTick(); // client thread, every tick
void* localClientCharacter();
void* localServerCharacter();
bool entityTransform(const void* entity, gn_transform& out);
int teleport(void* serverCharacter, const gn_transform* to);
void* getComponent(void* character, const gn_type* type);
std::string levelName();
int onLevel(const std::string& mod, gn_level_fn fn, void* user);
void pollLevel(); // GridNode's thread, about every 0.1 s

// Overlay (ui.cpp).
namespace ui {
struct Settings {
    bool enabled = true;
    int menuKey = 0x2D; // VK_INSERT
    float scale = 1.f;
    bool inspector = true;
};
bool install(const Settings& s);
int onUi(const std::string& mod, gn_ui_fn fn, void* user);
void* context();
void allocators(void** alloc, void** free, void** user);
int check(const char* mod, const char* version, size_t io, size_t style, size_t vec2, size_t vec4, size_t vert, size_t idx);
bool menuOpen();
void toggleMenu();
std::string keyName(int vk);
int keyFromName(const std::string& name);
} // namespace ui

// Inspector (inspector.cpp): start() tracks entities from then on; draw() on the render thread.
namespace inspector {
void start();
void draw(bool* open);
} // namespace inspector

} // namespace gn::host
