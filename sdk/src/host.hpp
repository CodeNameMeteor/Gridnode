#pragma once
// Internal state of the GridNode runtime inside the game (not part of the public API).

#include <atomic>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "gridnode/api.h"
#include "gridnode/dispatch.hpp"
#include "gridnode/hook_table.hpp"
#include "gridnode/reflect/registry.hpp"

namespace gn::host {

// GridNode's part of a hook (everything but the call to the game's original): an exception never
// leaves into the engine's frames (it is logged, the first few times per site, and dropped).
void logHookException(const char* where, const char* what);
template <class F> void guardHook(const char* where, F&& f) noexcept {
    try {
        f();
    } catch (const std::exception& e) {
        logHookException(where, e.what());
    } catch (...) {
        logHookException(where, nullptr);
    }
}

// Threads in a module's code (quiet.cpp), before it is freed: any other thread running there or with
// an address in it on the used part of its stack (paused while looked at).
struct ThreadsInCode {
    int threads = 0;
    unsigned long firstThread = 0;
    uintptr_t firstAt = 0;
    bool onStack = false;    // found by scanning a stack (maybe stale), not by unwinding it
    bool inRegister = false; // found in a register (about to be called)
    unsigned long pausedMs = 0;
};
ThreadsInCode threadsInCode(void* module, bool registers); // an HMODULE; registers: check them too
// Looks again every 100 ms for up to ms; false (logged, as `what`) when threads are still there.
bool waitCodeQuiet(void* module, const char* what, unsigned long ms);

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
// Wraps every entity class's deleting destructor (vtable +0x38) so a destroyed entity is marked
// dead at once (inspector::alive is false from then on). Once, at startup.
void hookDestructors();
uint64_t destroyedCount(); // entities destroyed since then
int onDestroyed(const std::string& owner, const std::string& className, gn_destroyed_fn fn, void* user); // API 26

// The API table handed to mods (api.cpp).
const gn_api* api();

// Mods (mods.cpp): loads every DLL in `folder` that exports gn_mod_load.
void loadMods(const std::wstring& folder);
struct ModInfo {
    std::string file;
    bool loaded = false;
    int result = 0;
    bool reloadable = false;  // exports gn_mod_unload: loaded from a copy, can be reloaded
    bool autoReload = false;  // reloaded when its file changes
    bool inMemory = false;    // its DLL is loaded (false after Unload: Load brings it back)
    std::string status;       // the last reload, for the menu
    // From its manifest (gn_mod_info); id defaults to the file name without .dll.
    std::string id, name, version, author, description, depends;
    std::string online;       // the reason it gave online_allow: it changes online services on purpose ("" if not)
    std::vector<std::string> shares; // hooks it shares with other mods: "function 0x... with other-mod"
};
std::vector<ModInfo> modList();
void noteSharedHook(const std::string& mod, uint64_t slot); // logs when another mod hooks that slot too
void modReload(const std::string& file);             // any thread: done on GridNode's thread
void modAutoReload(const std::string& file, bool on);
void modUnload(const std::string& file);             // any thread: unloaded on GridNode's thread, stays unloaded
void modLoad(const std::string& file);               // any thread: an unloaded mod loaded again
int modUnloadAt(const void* address);                // the mod whose code holds address (gn_api mod_unload)
void pollMods();                                     // GridNode's thread: reloads asked for or due
int modExport(const void* caller, const char* name, void* fn);          // gn_api mod_export
int modDir(const void* caller, char* out, size_t size);                 // gn_api mod_dir
void* modImport(const void* caller, const char* modId, const char* name); // gn_api mod_import
int modVersion(const char* modId, char* out, size_t size);               // gn_api mod_version
// A callback queued for later may only run while its mod is loaded: false once fn's mod was unloaded.
bool modCodeLive(const void* fn);
// Online services (gn_api online_allow): a mod says it changes them on purpose (restoring or
// replacing online features). Until it does, GridNode refuses its online console commands. Code that is not a mod's (GridNode's own menu, which asks the player)
// counts as allowed. onlineAllow logs a warning; the reason is shown in the menu's Mods list.
int onlineAllow(const void* caller, const char* mod, const char* reason);
bool onlineAllowed(const void* caller);
std::string modNameAt(const void* caller); // the id of the mod whose code holds caller, or 

// Declared mod settings (modsettings.cpp): gn_api setting_*; settingComments for the settings file.
int settingAdd(const char* mod, const gn_setting* s, const void* caller);
double settingNumber(const char* mod, const char* id);
int settingText(const char* mod, const char* id, char* out, size_t size);
int settingSetNumber(const char* mod, const char* id, double v);
int settingSetText(const char* mod, const char* id, const char* v);
int settingOnChange(const char* mod, const char* id, gn_setting_fn fn, void* user);
int settingsPage(const char* mod, int shown);
std::vector<std::pair<std::string, std::string>> settingComments(const std::string& mod);
void drawSettingsPages(); // render thread, in the GridNode menu
bool unloadAllMods(); // GridNode's thread, for a full unload: true when every mod was freed (the rest are inert)

// A loaded module's code, to take out everything a mod registered when it is unloaded: each part of
// GridNode drops the callbacks (and hooks) whose function is in it.
struct CodeRange {
    uintptr_t begin = 0, end = 0;
    bool has(const void* p) const { return reinterpret_cast<uintptr_t>(p) >= begin && reinterpret_cast<uintptr_t>(p) < end; }
};
void forgetSettings(const CodeRange& r); // a mod's declared settings and listeners (modsettings.cpp)
void forgetTicks(const CodeRange& r);    // at the next tick of each realm (server().removing())
size_t forgetTasks(const CodeRange& r);  // its queued run_on_server / run_on_client tasks, now (just before its DLL is freed)
bool taskLive(const thread::Dispatcher::Task& t); // false for a task into a mod being unloaded
size_t forgetHooks(const CodeRange& r, std::vector<std::string>* refused);
void forgetCreated(const CodeRange& r);
void forgetDestroyed(const CodeRange& r);
void forgetLevelReady(const CodeRange& r);
void forgetLevel(const CodeRange& r);
void forgetMessages(const CodeRange& r);
void forgetMovement(const CodeRange& r);
void forgetEvents(const CodeRange& r);

// Code patches (patches.cpp): gn_api patch_bytes / patch_call; undone when their mod (the code that
// made them: caller) is unloaded, or all at a full unload.
int patchBytes(const char* mod, uint64_t va, const void* expected, const void* bytes, size_t size, const void* caller);
int patchCall(const char* mod, uint64_t siteVa, uint64_t expectedTargetVa, void* detour, void** original, const void* caller);
// Function detours (patches.cpp): gn_api hook_function. The detours go in the hook table (by slot);
// these tell the table's slot writer and the Hooks list about the slots.
int hookFunction(const char* mod, uint64_t fnVa, void* detour, void** original);
void patchesReserve(); // pages near the game's code, taken at startup (patches.cpp)
bool isDetourSlot(uint64_t slot);
uint64_t detourFunction(uint64_t slot); // the function a detour slot belongs to (va), or 0
size_t forgetPatches(const CodeRange& r);
size_t removeAllPatches();
struct PatchInfo {
    std::string mod;
    uint64_t va;
    size_t size;
    std::string what;
};
std::vector<PatchInfo> patchList();
std::vector<std::pair<uintptr_t, uint64_t>> patchOriginals(); // for an image dump: the original bytes

// Unloading GridNode (unload.cpp): everything it and the mods changed is put back, then gridnode.dll
// is freed. GridNode's own short-lived threads count themselves so the unload waits for them.
void unloadRequest();   // any thread: done on GridNode's thread
bool unloadRequested();
bool unloading();       // true once it started: start nothing new
bool unloaded();        // its hooks are out: a detour still called (another hook over it) only passes on
bool unloadEverything(); // GridNode's thread: true when gridnode.dll can be freed now
struct BusyThread {     // in a thread of ours: the unload waits until it ends
    BusyThread();
    ~BusyThread();
    BusyThread(const BusyThread&) = delete;
    BusyThread& operator=(const BusyThread&) = delete;
};

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
int onLevelReady(const std::string& mod, gn_level_ready_fn fn, void* user); // API 26
int levelReady();                                                      // 1: the current level is ready
int groundBelow(const float pos[3], float depth, float* groundY);      // server thread: a ray down
void levelReadyServerTick();
// Server thread: a ray's closest hit and the physics object it hit (0 when unknown).
int raycastObject(const float from[3], const float to[3], gn_ray_hit* out, uintptr_t* object, uint32_t* body, uint32_t* part);
// A physics entity's collision bodies: (id, its RigidBody or 0 when it is another kind), at most 64.
std::vector<std::pair<uint32_t, uintptr_t>> physicsBodies(uintptr_t entity);
std::pair<uintptr_t, const reflect::Type*> entityOfBody(uint32_t body); // the server physics entity with that body
int apiRaycastObject(const float from[3], const float to[3], gn_ray_hit_ex* out);
int apiEntityBodies(void* entity, uint32_t* ids, int max);
int apiBodyTransform(void* entity, uint32_t body, gn_transform* out);
int apiBodySetTransform(void* entity, uint32_t body, const gn_transform* to);
void* apiEntityOtherHalf(void* entity);
// Collision boxes (bodies.cpp) and what a Havok body says.
void bodiesStart();
int boxAdd(const char* mod, const gn_transform* at, const float* half, uint32_t* out, const void* caller);
int boxMove(uint32_t box, const gn_transform* at);
int boxRemove(uint32_t box);
int boxState(uint32_t box);
void forgetBoxes(const CodeRange& r); // a mod's boxes (made from its code)
bool bodiesRelease(unsigned long ms);
bool bodyFilter(uintptr_t object, uint32_t& filter, uint32_t& material);
// Sounds (sounds.cpp).
int soundPlay(const char* mod, const char* path, const char* event, const float* pos, uint32_t* out, const void* caller);
int soundEvent(uint32_t sound, const char* event);
int soundMove(uint32_t sound, const float* pos);
int soundPlaying(uint32_t sound);
int soundStop(uint32_t sound);
void forgetSounds(const CodeRange& r); // a mod's sounds; an empty range: every one
int characterDespawn(void* entity);
int apiBodyVelocity(void* entity, uint32_t body, float* linear, float* angular);
int apiBodySetVelocity(void* entity, uint32_t body, const float* linear, const float* angular);
int apiBodyPush(void* entity, uint32_t body, const float* change);
int physicsGravity(int realm, float* out);
int physicsSetGravity(int realm, const float* g, const void* caller);
// gamestate.cpp (API 22)
int characterHealth(void* character, float* health, float* maxHealth); // server thread
int characterSetHealth(void* character, float health);                 // server thread
int waypoint(float* out);
int checkpoint(float* out);
int progressFlags(gn_progress_flag_fn fn, void* user);
int progressFind(const char* name, gn_progress_flag* out);
int progressSet(void* flag, int32_t value, int32_t* now); // server thread
int mission(int32_t* index, char* name, size_t size);
std::string progressCommand(const std::string& args); // the console's "progress [FILTER | NAME VALUE]"
void forgetGravity(const CodeRange& r);
bool cameraTransform(gn_transform& out); // the local player's camera
int onLevel(const std::string& mod, gn_level_fn fn, void* user);
void pollLevel(); // GridNode's thread, about every 0.1 s

// Faith, keys, tuning, game state (faith.cpp).
int keyFind(const char* name);
int keyPress(int key, uint32_t ticks);
int jump();
int actionPress(uint32_t hash, uint32_t ticks);
int actionHeld(uint32_t hash); // the player's live input: 1 / 0, or -GN_ERR_*
// ANT's types and Faith's move tracker (ant.cpp, API 25).
void antTick(); // client tick: reads ANT's registry once it is complete
int antTypeCount();
int antTypeAt(uint32_t i, gn_ant_type* out);
int antTypeFind(const char* name, gn_ant_type* out);
int antFieldAt(const char* type, uint32_t i, gn_ant_field* out);
int antFieldFind(const char* type, const char* name, gn_ant_field* out);
int moveTracker(gn_move_tracker* out);
int antLiveField(const char* type, const char* field, gn_ant_live_field* out); // API 26, client thread
int antObjects(const char* type, void** out, uint32_t max);                     // API 26, client thread
int replayRecent(gn_replay_info* info, float* times, gn_replay_bone* bones, uint32_t maxFrames); // API 27 (replay.cpp)
std::string replayCommand(); // the console's "replay"
std::string antCommand(const std::string& args); // the console's "ant [FILTER | TYPE]"
void faithClientTick(); // client thread: applies and releases held keys
int movementGet(gn_movement* out);
int jumpHeld();       // the player's jump action: 1 held, 0 not, -1 unavailable
int landingPrediction(gn_landing* out); // landing.cpp
int jumpHeldByMods(); // GridNode holds jump for a mod (gn::jump): 1 or 0
float oldTrainerSpeed();  // research: the user's old trainer's speed path (-1 none)
float speedProbeSample(); // research: her measured horizontal speed (m/s, -1 none); scores speed candidates
int movementSet(const gn_movement* in, uint32_t fields);
int onMovement(const std::string& mod, gn_movement_fn fn, void* user);
void movementEventsClientTick(); // client thread: movement events to the mods listening
uint32_t tuningCount();
int tuningAt(uint32_t index, gn_tuning* out);
int tuningSet(const char* name, float value);
int gameStateGet(gn_game_state* out);
int timeOfDaySet(float seconds);
int positionSet(const float pos[3]);
int godModeSet(int on); // server thread
void faithRelease();     // a full unload: held keys and actions let go (client thread), god mode as it was (server thread)

// Mod settings (config.cpp): mods/<mod>.ini.
void configStart(const std::wstring& folder); // the mods folder
int configGet(const char* mod, const char* key, char* out, size_t size);
int configSet(const char* mod, const char* key, const char* value);
std::string configValue(const std::string& mod, const std::string& key, bool* found);
void configFlush(bool now); // GridNode's thread: writes settings changed over a second ago (all, when now)

// Hotkeys (hotkeys.cpp): mods' keys, rebound in the mods' own windows, stored in their settings.
int hotkeyAdd(const char* mod, const char* id, const char* label, int defaultVk, gn_hotkey_fn fn, void* user);
int hotkeyInfo(const char* mod, const char* id, gn_hotkey_info* out);
int hotkeyRebind(const char* mod, const char* id);
int hotkeySet(const char* mod, const char* id, int vk);
namespace hotkeys {
void frame(bool menuOpen); // render thread, every frame
void forget(const CodeRange& r);
} // namespace hotkeys

// Assets (assets.cpp).
void* assetFind(const char* path, uint32_t* group);
int entitiesNear(const float pos[3], float radius, gn_entity_fn fn, void* user); // world.cpp
int entitySetTransform(void* entity, const gn_transform* to);
int entitiesOfClass(const char* className, gn_entity_each_fn fn, void* user); // world.cpp; how many, or -GN_ERR_*
void assetRelease(void* asset);

// Layouts (layouts.cpp, M7): level edits applied at runtime, and their editor.
namespace layouts {
void start(const std::wstring& gridnodeDir);
void onLevel(const std::string& level); // GridNode's thread, when a level starts
void serverTick();
void clientTick();
void draw(bool* open); // render thread
void frame(bool menuOpen, bool editor); // render thread, every frame: the free camera (and with editor, the editor's keys and picking)
int freecamSet(bool on);                  // mods: the free camera on / off (next frame)
int freecamGet(gn_freecam* out);
int teleportRequest(int to, bool keepFlying); // 1 the crosshair, 2 the free camera's position (next frame)
int cameraSet(const void* caller, const gn_transform* t, uint32_t flags); // API 27: a mod's camera (null: given back)
int cameraFovSet(const void* caller, float degrees);                     // API 27: 0 gives the FOV back
void forgetCamera(const CodeRange& r);                                    // a mod unloading: its camera and FOV given back
std::string cameraCommand(const std::string& args);                      // the console's "camera"
bool view(gn_view& out);   // render thread: the camera as drawn (the free camera while it flies)
void release(); // a full unload: the free camera off, copies removed, moved magropes given their own transforms back
} // namespace layouts

// Events between entities (events.cpp): onEvent (vtable +0x20) wrapped in every entity class, for the
// Recorder (its tap) and mods (on_event).
namespace events {
using Tap = void (*)(void* peer, void* event);
void start(const std::wstring& gridnodeDir); // where recordings\event-names.txt is
void setTap(Tap tap);                        // the Recorder; called for every event while set
bool ensureHooked(const char* who);          // wraps them (once); false when it could not
uint32_t hash(const std::string& name);      // an event id from its name (djb2-xor)
std::string name(uint32_t id);               // its name, or the id as hex
uint32_t idOf(const char* event);            // a name or "0x<hash>"; 0 for none
// Delivers the event to the entity's onEvent on its realm thread at the next tick (as the bus does);
// false when it has none. sendToClass: to every live entity of the class (and subclasses).
bool send(uintptr_t entity, const reflect::Type* type, uint32_t id);
std::vector<std::pair<uintptr_t, const reflect::Type*>> sendToClass(const reflect::Type* cls, uint32_t id);
} // namespace events
int eventSend(const std::string& mod, void* entity, const char* event);
int eventSendClass(const std::string& mod, const char* className, const char* event); // how many, or -GN_ERR_*
int onEvent(const std::string& mod, const char* event, const char* className, gn_event_fn fn, void* user);
int eventName(uint32_t id, char* out, size_t size);

// Recorder (recorder.cpp): messages, entity creations and markers with call stacks, exported as
// JSON lines for research (M8).
namespace fielddiff {
void draw(bool* open); // render thread
} // namespace fielddiff

namespace tracer {
bool add(const std::string& functionOrName); // render thread
void draw(bool* open);                       // render thread
void release();                              // a full unload: the stubs stop calling GridNode
} // namespace tracer

namespace recorder {
void start(const std::wstring& gridnodeDir);
void onLevel(const std::string& level);
void draw(bool* open);    // render thread
void frame();             // render thread, every frame: its keys
void onDestroyed(void* entity); // any game thread, from the entity destructor hooks (entities.cpp), before it goes
void release();                 // a full unload: stops recording (an export in progress finishes)
} // namespace recorder

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
void messageTap(MessageRecorder rec);               // every category to rec, null to stop (the recorder)
bool messagesAvailable();
void messagesClientTick(); // client thread: registrations with the core and client managers
void messagesServerTick(); // server thread: registrations with the server manager
void messagesRelease();    // a full unload: each manager's registrations are taken out at its next tick
bool messagesReleased();   // all taken out

// Overlay (ui.cpp).
namespace ui {
struct Settings {
    bool enabled = true;
    int menuKey = 0x2D; // VK_INSERT
    float scale = 1.f;
    bool inspector = true;
    // Developer tools ([tools] in gridnode.ini; all off, and not built, in a player build).
    bool editor = true;   // the Layout editor, free camera and their keys (layouts still load)
    bool recorder = true; // the Recorder
    bool console = true;  // the console window
    bool devInfo = true;  // the World / Hooks sections and Dear ImGui metrics
    bool gridnodeWindow = true; // the GridNode window in the menu ([ui] gridnode_window=0: only mods' windows)
};
bool install(const Settings& s);
int onUi(const std::string& mod, gn_ui_fn fn, void* user);
void forgetUi(const CodeRange& r);
uint64_t frameCount(); // frames drawn so far
bool release(); // a full unload: the overlay closed, Present, ResizeBuffers and the window procedure back; false if one could not be
void* context();
void allocators(void** alloc, void** free, void** user);
int check(const char* mod, const char* version, size_t io, size_t style, size_t vec2, size_t vec4, size_t vert, size_t idx);
bool menuOpen();
bool setMenu(bool open); // API 27 (false: no overlay): a mod opens or closes the menu (its own key)
bool gameFocused(); // the game's window is the foreground window (for keys read with GetAsyncKeyState)
void toggleMenu();
void setLookCapture(bool on);            // raw mouse movement goes to takeLook, not the cursor
void setInputCapture(bool on);           // free camera with the menu closed: the game gets no input
void setPointer(bool on);                // free camera with the menu closed: show the cursor (for handles); centres it when turned on
void takeLook(float& dx, float& dy);     // movement since the last call (raw counts)
std::string keyName(int vk);
std::vector<std::pair<int, std::string>> reservedKeys(); // GridNode's own keys (the menu, the tools'), for hotkey clashes
int keyFromName(const std::string& name);
bool isRenderThread(); // the thread drawing the overlay (Present)
} // namespace ui

// Inspector (inspector.cpp): start() tracks entities from then on; draw() on the render thread.
namespace crash {
void install(); // logs the first few faults: address, registers, the unwound call stack with names
void setModsFolder(const std::wstring& folder); // marks DLLs under it as mods in crash notes
void refreshModules(); // the module table the handler names frames from (after modules load; not in a fault)
void uninstall();
}

namespace inspector {
void start();
void draw(bool* open);
void drawConsole(bool* open); // the console window (render thread)
std::vector<std::pair<uintptr_t, const reflect::Type*>> tracked(); // entities GridNode saw created, not yet found dead
std::vector<std::pair<uintptr_t, const reflect::Type*>> trackedOf(const reflect::Type* base); // the same, only that class and its subclasses (no full copy)
bool alive(uintptr_t entity, const reflect::Type* type); // still that entity: not found dead since, and still of that type
void markDead(uintptr_t entity); // GridNode is destroying it: alive() is false from now on (until a new entity is made there)
} // namespace inspector

} // namespace gn::host
