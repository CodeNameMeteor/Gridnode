/* GridNode mod API (C ABI).
 *
 * A mod is a DLL in the game's mods/ folder (next to gridnode.dll) that exports
 *
 *     GN_MOD_EXPORT int gn_mod_load(const gn_api* api);
 *
 * GridNode calls it on its own thread once the build check has passed and the engine's types
 * are known. Return 0 on success. Keep `api`: it stays valid for the life of the process.
 * Mods are never unloaded.
 *
 * The API is a table of function pointers, so a mod can be built with any compiler (MSVC,
 * MinGW, clang) and needs no import library. C++ mods should use gridnode/mod.hpp, which wraps
 * this table.
 *
 * Addresses passed to and returned from the API are runtime addresses in the game process,
 * except where a name ends in _va (image VAs, as in Ghidra and gridnode/engine.hpp).
 */
#ifndef GRIDNODE_API_H
#define GRIDNODE_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GN_API_VERSION 11 /* 2: world, levels and UI (M4); 3: settings and console (M4.5); 4: messages, 5: ray casts (M5); 6: keys, movement, tuning, game state; 7: assets by name (M7); 8: landing prediction; 9: mod settings, hotkeys, movement events; 10: the view, for drawing in the world; 11: events between entities */

#if defined(_WIN32)
#define GN_MOD_EXPORT __declspec(dllexport)
#else
#define GN_MOD_EXPORT __attribute__((visibility("default")))
#endif

/* Results */
#define GN_OK 0
#define GN_ERR_ARGUMENT 1      /* a null or invalid argument */
#define GN_ERR_NOT_FOUND 2     /* no such type / field / class */
#define GN_ERR_SLOT 3          /* a hook slot does not hold what was expected */
#define GN_ERR_FULL 4          /* an internal table is full */
#define GN_ERR_UNAVAILABLE 5   /* the feature is not available (e.g. its hook point could not be installed) */
#define GN_ERR_THREAD 6        /* called on the wrong thread (see the function's comment) */
#define GN_ERR_MISMATCH 7      /* the mod was built against a different Dear ImGui */

typedef struct gn_type gn_type; /* a runtime TypeInfo / ClassInfo of the engine */

typedef void (*gn_tick_fn)(float dt, void* user);
typedef void (*gn_task_fn)(void* user);
typedef void (*gn_created_fn)(void* entity, void* creation_info, void* user);
/* loaded: 1 when a level starts (name e.g. "Levels/SP/SP_MainCity/SP_MainCity"), 0 when it ends. */
typedef void (*gn_level_fn)(const char* level, int loaded, void* user);
/* Every frame on the render thread, between Dear ImGui's NewFrame and Render. menu_open: the
 * GridNode menu (INSERT) is open, so the mouse and keyboard belong to the overlay. */
typedef void (*gn_ui_fn)(int menu_open, void* user);

/* The console's output for one command line (client thread). */
typedef void (*gn_console_fn)(const char* output, void* user);

#define GN_COMMAND_SETTING 1u /* a settings field bound to the console (Group.Field [value]) */
#define GN_COMMAND_ONLINE 2u  /* touches online play or EA services: see online_warning */
typedef struct gn_command {
    const char* name;
    const char* group;
    const char* full;   /* group.name, as typed in the console */
    const char* help;   /* may be empty */
    uint32_t flags;     /* GN_COMMAND_* */
} gn_command;

/* A message the engine sent. Messages are value types (gn_type) whose members after the 0x30-byte
 * header are mostly not reflected: read them at known offsets, or look at them in the inspector. */
typedef struct gn_message_info {
    const gn_type* type;      /* the message's type, or null if unknown */
    const char* type_name;    /* e.g. "ClientPlayerLocalSetMessage" ("" if unknown) */
    const char* category;     /* e.g. "ClientPlayer" */
    uint32_t type_id;         /* name hash of the type name */
    uint32_t category_id;     /* name hash of the category */
    uint32_t realm;           /* the message's realm filter (0xff: any) */
    int manager;              /* which message manager carried it: 0 core, 1 client, 2 server */
} gn_message_info;
/* Called on the thread that sends the message (usually its realm's game thread). The message is
 * only valid during the call. */
typedef void (*gn_message_fn)(const void* message, const gn_message_info* info, void* user);


/* The nearest thing a ray cast hit. */
typedef struct gn_ray_hit {
    float position[3];
    float normal[3];   /* the surface's normal at the hit */
    float distance;    /* metres from the start of the ray */
    float fraction;    /* 0..1 along the ray */
} gn_ray_hit;

/* The engine's LinearTransform: rotation rows and translation, each a float4 (w unused).
 * 16-byte aligned, as the engine reads it with aligned loads. */
typedef struct gn_transform {
#if defined(__cplusplus)
    alignas(16) float right[4];
#elif defined(_MSC_VER)
    __declspec(align(16)) float right[4];
#else
    float right[4] __attribute__((aligned(16)));
#endif
    float up[4];
    float forward[4];
    float trans[4];
} gn_transform;

/* An entity found by entities_near: its type, its world transform, its distance. */
typedef void (*gn_entity_fn)(void* entity, const gn_type* type, const gn_transform* at, float distance, void* user);

enum gn_category {
    GN_CATEGORY_UNKNOWN = 0,
    GN_CATEGORY_PRIMITIVE,
    GN_CATEGORY_VALUETYPE,
    GN_CATEGORY_CLASS,
    GN_CATEGORY_ARRAY,
    GN_CATEGORY_FIXEDARRAY,
    GN_CATEGORY_ENUM,
    GN_CATEGORY_OTHER
};

typedef struct gn_field {
    const char* name;
    uint32_t offset;          /* from the start of the object */
    uint32_t size;            /* bytes: pointers and arrays are 8 */
    uint16_t flags;           /* 0x48 array field, 0x2000 settings field */
    uint16_t category;        /* gn_category of the field's type */
    const gn_type* type;      /* the field's type */
    const gn_type* owner;     /* the class or value type that declares it (may be a base class) */
} gn_field;

/* Faith's movement (movement_get / movement_set).
 * Threads: movement_set, position_set, time_of_day_set, tuning_set, key_press and action_press write
 * at once, on the calling thread (the values are single words the game reads each tick). Values that
 * are not finite are refused with GN_ERR_ARGUMENT. */
typedef struct gn_movement {
    int32_t state;          /* the movement state: 1 on the ground, 2 jumping, 8 wall run (and the jump off it, until
                               landing), 3-4 other wall moves... (recorded 2026-10-30) */
    int32_t wall_count;     /* walls used since the ground */
    int32_t wall_climbs;    /* wall climbs used */
    int32_t wall_runs;      /* wall runs used */
    float last_ground_y;    /* height of the last ground; falls are measured from it (fall damage, stumbles) */
    int32_t on_ground;      /* 1 while she is in the AIR, 0 on the ground (the name is historical; recorded 2026-10-30) */
    float speed;            /* horizontal speed, m/s */
    float camera_forward[3];
} gn_movement;
/* The game's own landing prediction for Faith (landing_prediction): what the animation system works
 * out every tick while she is in the air (PositionPredictionProcessorAsset). */
typedef struct gn_landing {
    float ticks_to_hit;   /* 60ths of a second until she hits the ground. In testing it read 0.74-1.54 on
                             the tick before the landing (2-3 two ticks before): the tick to press jump on
                             for a bhop. Not reliable near walls and edges (it can drop early and hover). */
    float hit_height;     /* world height (Y) of the predicted hit */
    int32_t soft_landing; /* the game expects a soft landing: she cannot jump out of it (no bhop) */
    uint32_t age_ms;      /* since the game last predicted for her (it only does while she is in the air) */
    uint32_t count;       /* predictions so far: changes each time the game predicts */
} gn_landing;

#define GN_MOVE_STATE 0x1u
#define GN_MOVE_WALL_COUNT 0x2u
#define GN_MOVE_WALL_CLIMBS 0x4u
#define GN_MOVE_WALL_RUNS 0x8u
#define GN_MOVE_LAST_GROUND_Y 0x10u
#define GN_MOVE_SPEED 0x20u

/* A change in Faith's movement (on_movement), seen on a client tick. On one tick a landing or take-off
 * comes first, then the state change: a bhop's landing tick is LANDED then STATE (8 -> 0). */
#define GN_MOVE_EVENT_STATE 1    /* the movement state changed: old_state -> state */
#define GN_MOVE_EVENT_LANDED 2   /* she touched the ground (in the air -> not) */
#define GN_MOVE_EVENT_AIRBORNE 3 /* she left the ground */
typedef struct gn_movement_event {
    int32_t kind;      /* GN_MOVE_EVENT_* */
    int32_t state;     /* the movement state now */
    int32_t old_state; /* the movement state at the tick before */
    int32_t in_air;    /* 1 in the air now, 0 on the ground */
} gn_movement_event;
typedef void (*gn_movement_fn)(const gn_movement_event* e, const gn_movement* now, void* user);

/* The camera as drawn this frame (view_get), to put world points on the screen: gridnode/projection.hpp
 * does the math, gn::ui::line3d and friends (gridnode/ui.hpp) draw with it. Frostbite is y up. */
typedef struct gn_view {
    float position[3]; /* the camera, world space */
    float forward[3];  /* the way it looks (unit) */
    float right[3];    /* unit */
    float up[3];       /* unit */
    float fov_y;       /* vertical field of view, degrees */
    float width;       /* the screen, pixels (the overlay's display size) */
    float height;
} gn_view;

/* An event an entity received (on_event), before it handles it: the entity, its type (null if unknown)
 * and the event's id (the djb2-xor hash of its name; gn::eventHash in mod.hpp). On the thread that
 * delivered it, usually the entity's realm thread (server or client); keep it short. */
typedef void (*gn_event_fn)(void* entity, const gn_type* type, uint32_t event_id, void* user);

/* A mod's hotkey went down (hotkey_add). Client thread. */
typedef void (*gn_hotkey_fn)(void* user);
/* A hotkey as it is now (hotkey_info), for the mod's own UI. */
typedef struct gn_hotkey_info {
    int32_t vk;         /* its key (a Windows virtual-key code), 0 for none */
    int32_t default_vk; /* the key the mod gave it */
    int32_t waiting;    /* 1 while hotkey_rebind waits for a key */
    char name[32];      /* the key's name: "F", "F8", "none" */
    char clash[192];    /* what else is on that key ("Save position, other-mod: X, GridNode: the menu"), "" for nothing */
} gn_hotkey_info;

/* Input actions (action_press). */
#define GN_ACTION_JUMP 0xb5a0a981u

/* A movement tuning value (run, jump, wall run/climb, coil, slide, mag rope). */
typedef struct gn_tuning {
    const char* name;  /* e.g. "MaxRunSpeed" */
    const char* group; /* e.g. "Running" */
    float value;
    float default_value;
    int32_t available; /* 0 until GridNode has seen the default at the value's address this session */
} gn_tuning;

/* What MEC_Trainer read about the game (game_state). */
typedef struct gn_game_state {
    int32_t loading;
    int32_t in_menu;
    int32_t content_active; /* a shared content event (a mission, a dash) is running */
    int32_t dash_started;
    float time_of_day;      /* seconds; -1 when there is no clock */
} gn_game_state;

typedef struct gn_api {
    uint32_t version;         /* GN_API_VERSION of the host */
    uint32_t size;            /* sizeof(gn_api) of the host: newer hosts only append */
    const char* gridnode_version;

    /* ---- general -------------------------------------------------------------------------- */
    void (*log)(const char* mod, const char* message);  /* one line into gridnode.log */
    uint64_t (*module_base)(void);                      /* the game's image base in this process */
    uint64_t (*rva)(uint64_t va);                        /* image VA -> runtime address */

    /* ---- threads ---------------------------------------------------------------------------
     * The game runs its server and client worlds on separate threads, even in single player.
     * Engine functions must be called on the thread of their realm: use these callbacks. */
    int (*on_server_tick)(gn_tick_fn fn, void* user);   /* every server tick, after the world update */
    int (*on_client_tick)(gn_tick_fn fn, void* user);   /* every client tick, after the world update */
    void (*run_on_server)(gn_task_fn fn, void* user);    /* once, at the next server tick */
    void (*run_on_client)(gn_task_fn fn, void* user);    /* once, at the next client tick */
    int (*is_server_thread)(void);
    int (*is_client_thread)(void);

    /* ---- hooks -----------------------------------------------------------------------------
     * Swaps the pointer at slot (a vtable slot, creator slot or game-owned function pointer).
     * expected_va: what the slot must hold before any hook (an image VA); 0 accepts any function
     * in the game image. Hooks on the same slot chain: *original receives what the slot held
     * (the game's function or the previous mod's detour); always call through it.
     * Game code is never patched. Hooks stay for the life of the process. */
    int (*hook_slot)(const char* mod, uint64_t slot_va, uint64_t expected_va, void* detour, void** original);

    /* ---- reflection ----------------------------------------------------------------------- */
    const gn_type* (*type_find)(const char* name);       /* by name, e.g. "PamClientCharacterEntity" */
    const gn_type* (*type_of)(const void* object);       /* from the object's vtable; null if unknown */
    int (*is_kind_of)(const gn_type* type, const gn_type* base);
    const char* (*type_name)(const gn_type* type);
    uint32_t (*type_size)(const gn_type* type);
    int (*type_category)(const gn_type* type);
    const gn_type* (*type_super)(const gn_type* type);
    uint64_t (*type_va)(const gn_type* type);           /* the TypeInfo's image VA */
    /* Looks up a field by name in the type and its base classes. */
    int (*field_find)(const gn_type* type, const char* name, gn_field* out);
    /* Fields declared by the type itself (not its bases): index 0..count-1. */
    uint32_t (*field_count)(const gn_type* type);
    int (*field_at)(const gn_type* type, uint32_t index, gn_field* out);
    /* Fault-tolerant memory access (returns GN_OK or GN_ERR_ARGUMENT). */
    int (*read)(const void* address, void* out, size_t size);
    int (*write)(void* address, const void* data, size_t size);

    /* ---- entities --------------------------------------------------------------------------
     * Calls fn for every entity of class_name (or a subclass) the game creates from now on,
     * right after it was constructed, on the thread that created it. Works for every class
     * the engine has an entity creator for. */
    int (*on_created)(const char* mod, const char* class_name, gn_created_fn fn, void* user);

    /* ==== GN_API_VERSION 2 ==================================================================== */

    /* ---- world -----------------------------------------------------------------------------
     * Entities are runtime objects of the game: an entity pointer is only good while the entity
     * lives (until its level ends, for most). Reads below are fault tolerant; calls into the game
     * are made only on the thread the comment names. */
    void* (*local_client_character)(void);  /* Faith on the client (PamClientCharacterEntity); null if none. Client thread. */
    void* (*local_server_character)(void);  /* Faith on the server (ServerCharacterEntity); null if none. Any thread (reads only). */
    /* A spatial entity's world transform (any thread; GN_ERR_ARGUMENT if unreadable). */
    int (*entity_transform)(const void* entity, gn_transform* out);
    /* Moves a server character with the game's own teleport (velocity cleared, replicated to the
     * client). Server thread only: GN_ERR_THREAD otherwise; use run_on_server. */
    int (*teleport)(void* server_character, const gn_transform* to);
    /* The entity's component of this type (or a subclass), e.g. type_find("PamClientReplayRecordingComponent").
     * Characters only. Thread of the entity's realm. Null if none. */
    void* (*get_component)(void* character, const gn_type* type);
    /* The running level's asset name into out (empty between levels). Returns the length. Any thread. */
    size_t (*level_name)(char* out, size_t size);
    /* Called when a level starts or ends, on GridNode's own thread (within ~0.1 s). If a level is
     * already running, fn is called for it at once. */
    int (*on_level)(const char* mod, gn_level_fn fn, void* user);

    /* ---- UI (Dear ImGui overlay) --------------------------------------------------------------
     * GridNode draws a Dear ImGui overlay on the game (INSERT opens its menu). Mods draw into the
     * same frame with their own copy of Dear ImGui (third_party/imgui, same version): before
     * drawing they set GridNode's context and allocators. gridnode/ui.hpp does all of this.
     * The UI runs on the render thread: never call engine functions from it (use run_on_*). */
    int (*on_ui)(const char* mod, gn_ui_fn fn, void* user);
    void* (*ui_context)(void);              /* ImGuiContext*; null until the overlay has started */
    void (*ui_allocators)(void** alloc_fn, void** free_fn, void** user); /* for ImGui::SetAllocatorFunctions */
    /* GN_OK if the mod's Dear ImGui matches GridNode's (IMGUI_VERSION and the sizes ImGui itself
     * checks in IMGUI_CHECKVERSION); GN_ERR_MISMATCH otherwise (logged). */
    int (*ui_check)(const char* mod, const char* imgui_version, size_t io, size_t style, size_t vec2, size_t vec4,
                    size_t draw_vert, size_t draw_idx);
    int (*ui_menu_open)(void);

    /* ==== GN_API_VERSION 3 ==================================================================== */

    /* ---- settings ------------------------------------------------------------------------------
     * The engine's settings objects (GameTimeSettings, WindowSettings, ... 135 classes), all of
     * their reflected fields, not only those the console exposes. Read and write them with the
     * reflection functions above. The console may replace an object with a changed copy, so look
     * it up again rather than keeping the pointer. */
    void* (*settings_find)(const char* class_name);     /* null if the engine has none */
    uint32_t (*settings_count)(void);
    int (*settings_at)(uint32_t index, const gn_type** type, void** object);
    /* Why changing this settings class or console group could affect online play / EA services,
     * or null. GridNode never blocks these: it logs every change mods make to them. */
    const char* (*online_warning)(const char* class_or_group);

    /* ---- console -------------------------------------------------------------------------------
     * Runs one console line as the engine runs its config files, so every command works, also the
     * ones the player's console refuses. Queued to the client thread; fn (may be null) gets the
     * output there. GN_ERR_UNAVAILABLE if the console functions are not as expected. */
    int (*console_run)(const char* mod, const char* line, gn_console_fn fn, void* user);
    uint32_t (*console_command_count)(void);
    int (*console_command_at)(uint32_t index, gn_command* out);

    /* ==== GN_API_VERSION 4 ==================================================================== */

    /* ---- messages ------------------------------------------------------------------------------
     * Listens to the engine's messages, the way the game's own systems do (GridNode registers a
     * listener with the engine's message managers; nothing is hooked). type_or_category: a message
     * type ("ClientPlayerLocalSetMessage") or a whole category ("ClientPlayer"). Listening only
     * reads: it is safe for every category, online ones included. */
    int (*on_message)(const char* mod, const char* type_or_category, gn_message_fn fn, void* user);

    /* ==== GN_API_VERSION 5 ==================================================================== */

    /* ---- ray casts -----------------------------------------------------------------------------
     * Casts from..to through the server world's physics, as the game's own ray cast entities do.
     * radius > 0 sweeps a sphere. GN_OK: hit (out filled), GN_ERR_NOT_FOUND: nothing in the way,
     * GN_ERR_THREAD: not on the server thread (use run_on_server / on_server_tick),
     * GN_ERR_UNAVAILABLE: no level (or the caster is not as expected). */
    int (*raycast)(const float from[3], const float to[3], float radius, gn_ray_hit* out);

    /* ==== GN_API_VERSION 6 ==================================================================== */

    /* ---- keys ----------------------------------------------------------------------------------
     * The keyboard as the game reads it. key: an InputDeviceKeys value (key_find("Space") or
     * "IDK_Space"; -1 if unknown). key_press holds it for `ticks` client ticks and then releases it
     * (unless you are holding it yourself); 0 releases it at the next tick. Any thread. */
    int (*key_find)(const char* name);
    int (*key_press)(int key, uint32_t ticks);
    /* Makes Faith jump: the jump action for four ticks, whatever key the player bound to it; the
     * game decides what it becomes (a jump, a vault, a coil...). Falls back to the Space key. */
    int (*jump)(void);

    /* ---- Faith's movement ----------------------------------------------------------------------
     * GN_ERR_UNAVAILABLE when there is no Faith. movement_set writes the fields in `fields`
     * (GN_MOVE_*). Any thread; values are read and written as the game keeps them. */
    int (*movement_get)(gn_movement* out);
    int (*movement_set)(const gn_movement* in, uint32_t fields);

    /* ---- movement tuning -----------------------------------------------------------------------
     * The values MEC_Trainer's movement sliders change. tuning_set refuses (GN_ERR_UNAVAILABLE)
     * until GridNode has seen the value's default at its address, so it never writes to an
     * unexpected object. The values are shared by every character and stay until changed back. */
    uint32_t (*tuning_count)(void);
    int (*tuning_at)(uint32_t index, gn_tuning* out);
    int (*tuning_set)(const char* name, float value);

    /* ---- game state ------------------------------------------------------------------------------ */
    int (*game_state)(gn_game_state* out);
    int (*time_of_day_set)(float seconds); /* the level's clock; GN_ERR_UNAVAILABLE without one */

    /* Moves Faith by writing the position the animation system holds for her (no teleport: the
     * camera and her state carry on; physics may push her back out of geometry). For noclip-style
     * movement every tick. For a normal move use teleport. Any thread. */
    int (*position_set)(const float pos[3]);

    /* God mode: sets the IsImmortal property of Faith's health component, as the game's own logic
     * would (put back when turned off). A respawn makes a new component: call it again while god
     * mode should stay on (cheap when nothing changed). Falls still kill: that is not damage in MEC
     * (see GN_MOVE_LAST_GROUND_Y). Server thread (run_on_server / on_server_tick). */
    int (*god_mode)(int on);

    /* Holds an input action (by its hash, e.g. GN_ACTION_JUMP) for `ticks` client ticks, as if its
     * key were down, whatever the binding. Digital actions only. GN_ERR_NOT_FOUND: no such action,
     * GN_ERR_UNAVAILABLE: no Faith yet. Any thread. */
    int (*action_press)(uint32_t action_hash, uint32_t ticks);

    /* ==== GN_API_VERSION 7 ==================================================================== */

    /* ---- assets ----------------------------------------------------------------------------------
     * A loaded asset by its path ("Characters/FaithCharacter"): the primary instance of that
     * partition (a blueprint, a data container...), with a reference held for the caller; give it
     * back with asset_release. Null when no loaded resource group has it (assets are not loaded on
     * demand). group (may be null) receives the resource group it was found in. Client or server
     * thread. Use type_of to see what it is. */
    void* (*asset_find)(const char* path, uint32_t* group);
    void (*asset_release)(void* asset);

    /* ---- entities in place -----------------------------------------------------------------------
     * entities_near calls fn for every live entity (seen created since GridNode started) whose
     * transform can be read and is within radius of pos; returns how many. Any thread (it reads).
     * entity_set_transform moves a spatial entity as the game does (SpatialEntity_setTransform);
     * on its realm's thread (server entities: server thread; client: client thread). */
    int (*entities_near)(const float pos[3], float radius, gn_entity_fn fn, void* user);
    int (*entity_set_transform)(void* entity, const gn_transform* to);

    /* ==== GN_API_VERSION 8 ==================================================================== */

    /* ---- landing prediction ----------------------------------------------------------------------
     * The latest landing prediction the game made for Faith (gn_landing). The first call wraps the
     * prediction (pointer swaps); GN_ERR_UNAVAILABLE until the game has predicted for her (she has
     * been in the air) or if it cannot be read in this build. Any thread (it reads). */
    int (*landing_prediction)(gn_landing* out);

    /* ==== GN_API_VERSION 9 ==================================================================== */

    /* ---- mod settings ----------------------------------------------------------------------------
     * Values a mod keeps between runs, as key=value lines in <mods folder>/<mod>.ini (the mod name
     * made file-safe). Read when the mod first asks, written a second after the last change and when
     * the game closes. Keys: not empty, no '=' or line break, no spaces at either end; values: one
     * line (spaces at either end are dropped). config_get: GN_ERR_NOT_FOUND when unset (out is ""),
     * GN_ERR_FULL when out was too small (the value is cut to fit). Any thread. */
    int (*config_get)(const char* mod, const char* key, char* out, size_t size);
    int (*config_set)(const char* mod, const char* key, const char* value);

    /* ---- hotkeys ---------------------------------------------------------------------------------
     * A key the player can rebind, kept in the mod's own settings as key.<id>; the mod shows it in its
     * own window (gn::ui::hotkeyButton in gridnode/ui.hpp), so its players need no GridNode tool.
     * default_vk: a Windows virtual-key code ('F', 0x70 for F1), 0 for none. fn runs on the client
     * thread when the key goes down, while the game window has focus and the menu is closed (keys are
     * read every frame, so a short tap is seen). The same id again replaces the hotkey.
     * hotkey_rebind: waits for the next key the player presses (Escape: none, Backspace: the
     * default); hotkey_set: a key directly (-1: the default). GN_ERR_NOT_FOUND for no such hotkey.
     * Any thread. Needs the overlay ([ui] enabled, the default). */
    int (*hotkey_add)(const char* mod, const char* id, const char* label, int default_vk, gn_hotkey_fn fn, void* user);
    int (*hotkey_info)(const char* mod, const char* id, gn_hotkey_info* out);
    int (*hotkey_rebind)(const char* mod, const char* id);
    int (*hotkey_set)(const char* mod, const char* id, int vk);

    /* ---- movement events -------------------------------------------------------------------------
     * fn for every change in Faith's movement (gn_movement_event), on the client thread, on the tick it
     * happens; `now` is her movement on that tick. Read only while some mod listens. */
    int (*on_movement)(const char* mod, gn_movement_fn fn, void* user);

    /* ==== GN_API_VERSION 10 =================================================================== */

    /* ---- the view --------------------------------------------------------------------------------
     * The camera as the game draws this frame (the layout editor's free camera while it flies), with
     * the screen size, for drawing at world positions. Render thread (a UI callback);
     * GN_ERR_UNAVAILABLE with no camera (loading, menus) or no overlay. */
    int (*view_get)(gn_view* out);

    /* ==== GN_API_VERSION 11 =================================================================== */

    /* ---- events between entities -----------------------------------------------------------------
     * Entities talk through events on their buses ("StartReplay", "OnTriggered", "StreamIn"...): logic
     * fires one, the bus delivers it to each connected entity's onEvent. on_event calls fn for every
     * event delivered that matches: event, a name or its hash as hex ("0x2734a476"), null or "" for
     * every event; class_name, an entity class ("PamServerReplayEntity"; subclasses too), null or ""
     * for every class. GN_ERR_NOT_FOUND for an unknown class. The first call wraps onEvent in every
     * entity class (pointer swaps, shared with the Recorder). Every event of every entity is a lot:
     * filter by name where you can. Any thread.
     * event_name: an id's name, from the engine's type, field and enum names, our annotations and
     * recordings/event-names.txt; GN_ERR_NOT_FOUND (out "") for an unnamed id. Any thread. */
    int (*on_event)(const char* mod, const char* event, const char* class_name, gn_event_fn fn, void* user);
    int (*event_name)(uint32_t id, char* out, size_t size);
} gn_api;

typedef int (*gn_mod_load_fn)(const gn_api* api);

#ifdef __cplusplus
}
#endif

#endif /* GRIDNODE_API_H */
