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
int raycast(const float from[3], const float to[3], float radius, gn_ray_hit* out); // server thread
// Server thread: a ray's closest hit and the physics object it hit (0 when unknown).
int raycastObject(const float from[3], const float to[3], gn_ray_hit* out, uintptr_t* object, uint32_t* body, uint32_t* part);
bool cameraTransform(gn_transform& out); // the local player's camera
int onLevel(const std::string& mod, gn_level_fn fn, void* user);
void pollLevel(); // GridNode's thread, about every 0.1 s

// Faith, keys, tuning, game state (faith.cpp).
int keyFind(const char* name);
int keyPress(int key, uint32_t ticks);
int jump();
int actionPress(uint32_t hash, uint32_t ticks);
void faithClientTick(); // client thread: applies and releases held keys
int movementGet(gn_movement* out);
int movementSet(const gn_movement* in, uint32_t fields);
uint32_t tuningCount();
int tuningAt(uint32_t index, gn_tuning* out);
int tuningSet(const char* name, float value);
int gameStateGet(gn_game_state* out);
int timeOfDaySet(float seconds);
int positionSet(const float pos[3]);
int godModeSet(int on); // server thread

// Assets (assets.cpp).
void* assetFind(const char* path, uint32_t* group);
int entitiesNear(const float pos[3], float radius, gn_entity_fn fn, void* user); // world.cpp
int entitySetTransform(void* entity, const gn_transform* to);
void assetRelease(void* asset);

// Layouts (layouts.cpp, M7): level edits applied at runtime, and their editor.
namespace layouts {
void start(const std::wstring& gridnodeDir);
void onLevel(const std::string& level); // GridNode's thread, when a level starts
void serverTick();
void clientTick();
void draw(bool* open); // render thread
void frame(bool menuOpen); // render thread, every frame: the free camera
} // namespace layouts

// Settings and console (settings.cpp).
struct SettingsEntry {
    const reflect::Type* type = nullptr;
    uintptr_t object = 0;
    const char* online = nullptr; // why it touches online play, or null
};
struct ConsoleCommand {
    const char* name = "";
    const char* group = "";
    const char* help = "";
    const char* full = ""; // group.name
    bool setting = false;  // a bound settings field
    const char* online = nullptr;
};
std::vector<SettingsEntry> settingsList(); // cached for a second
uintptr_t settingsFind(const std::string& className);
const char* onlineWarning(const std::string& name); // settings class or console group
const char* commandOnline(const std::string& line);
bool consoleAvailable();
std::string consoleRun(const std::string& line); // client thread
std::vector<ConsoleCommand> consoleCommands();

// Messages (messages.cpp).
struct MessageType {
    const reflect::Type* type = nullptr;
    uint32_t typeId = 0;
    uint32_t category = 0;
    const char* categoryName = "";
    const char* online = nullptr;
};
using MessageRecorder = void (*)(int manager, const uint8_t* message, uint32_t category, uint32_t typeId, uint32_t realm);
const std::vector<MessageType>& messageTypes(); // every message type the engine has (sorted by name)
std::string messageCategoryName(uint32_t category);
const reflect::Type* messageTypeById(uint32_t typeId);
int onMessage(const std::string& mod, const std::string& typeOrCategory, gn_message_fn fn, void* user);
void messageMonitor(bool on, MessageRecorder rec); // every category to rec (the inspector)
bool messagesAvailable();
void messagesClientTick(); // client thread: registrations with the core and client managers
void messagesServerTick(); // server thread: registrations with the server manager

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
void setLookCapture(bool on);            // raw mouse movement goes to takeLook, not the cursor
void setInputCapture(bool on);           // free camera with the menu closed: the game gets no input
void takeLook(float& dx, float& dy);     // movement since the last call (raw counts)
std::string keyName(int vk);
int keyFromName(const std::string& name);
} // namespace ui

// Inspector (inspector.cpp): start() tracks entities from then on; draw() on the render thread.
namespace crash {
void install(); // logs the first few faults (address, registers, stack code addresses)
}

namespace inspector {
void start();
void draw(bool* open);
void drawConsole(bool* open); // the console window (render thread)
std::vector<std::pair<uintptr_t, const reflect::Type*>> tracked(); // every live entity GridNode saw created
} // namespace inspector

} // namespace gn::host
