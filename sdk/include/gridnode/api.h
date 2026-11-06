/* GridNode mod API (C ABI).
 *
 * A mod is a DLL in the game's mods/ folder (next to gridnode.dll) that exports
 *
 *     GN_MOD_EXPORT int gn_mod_load(const gn_api* api);
 *
 * GridNode calls it on its own thread once the build check has passed and the engine's types
 * are known. Return 0 on success. Keep `api`: it stays valid for the life of the process.
 *
 * A mod that also exports
 *
 *     GN_MOD_EXPORT int gn_mod_unload(void);
 *
 * can be reloaded while the game runs (the GridNode menu's Mods list, or when its DLL changes): it
 * is loaded from a copy in mods/.loaded/, so the DLL can be rebuilt meanwhile. gn_mod_unload runs on
 * GridNode's thread before the mod is unloaded: stop the mod's own threads, undo what GridNode cannot
 * see, return 0 (anything else: it stays loaded). GridNode then takes out every callback and hook
 * whose code is in the mod, waits for the game's threads to leave it, frees it, and loads it again.
 * Other mods are never unloaded.
 *
 * A mod may also say who it is (API 16):
 *
 *     GN_MOD_EXPORT const gn_mod_manifest* gn_mod_info(void);
 *
 * called before gn_mod_load: its id, version and dependencies. Mods are loaded dependencies first,
 * otherwise in file name order; a mod whose hard dependency is missing or too old is not started.
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

#define GN_API_VERSION 30 /* 2: world, levels and UI (M4); 3: settings and console (M4.5); 4: messages, 5: ray casts (M5); 6: keys, movement, tuning, game state; 7: assets by name (M7); 8: landing prediction; 9: mod settings, hotkeys, movement events; 10: the view, for drawing in the world; 11: events between entities; 12: sending events; 13: the free camera; 14: entities of a class; 15: unloading, code patches; 16: function detours, mod manifests and functions between mods; 17: declared settings; 18: what a ray hit, collision bodies; 19: collision boxes, sounds, despawning characters, ray hit layers; 20: a mod's folder; 21: body velocities, gravity; 22: health, the waypoint, the checkpoint, progression (the save's flags, missions); 23: online services on purpose (online_allow), view_get_ex; 24: the player's own input (action_held); 25: ANT types by name, Faith's move tracker; 26: destroyed entities, the level ready, ANT fields in live objects; 27: the camera for mods, Faith's last 15 seconds, mods open the menu; 28: holding the time of day; 29: the visual environment, timelines; 30: streaming focus points */
/* Offsets in the visual environment state (visual_env_*); GN_VE_SIZE bounds them. */
#define GN_VE_OUTDOOR_LIGHT 0x000u
#define GN_VE_TONEMAP 0x1d0u
#define GN_VE_COLOR_CORRECTION 0x330u
#define GN_VE_SKY 0x390u
#define GN_VE_WIND 0x6e0u
#define GN_VE_DEPTH_OF_FIELD 0xa50u
#define GN_VE_VIGNETTE 0xac0u
#define GN_VE_FILM_GRAIN 0xb00u
#define GN_VE_SIZE 0xb30u
#define GN_VE_ALL 0xffffffffu

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

/* What gn_mod_info returns (API 16; static, it is read before gn_mod_load). Every string may be null. */
typedef struct gn_mod_manifest {
    uint32_t size;              /* sizeof(gn_mod_manifest) */
    const char* id;             /* unique, e.g. "mec-trainer"; null: the file name without .dll */
    const char* name;           /* shown in the Mods list */
    const char* version;        /* dotted numbers, "1.2.0" */
    const char* author;
    const char* description;
    const char* const* depends; /* null-terminated: "id" (needed), "id>=1.2" (that version or newer),
                                   "?id" (optional: loaded after it when it is there) */
} gn_mod_manifest;
typedef const gn_mod_manifest* (*gn_mod_info_fn)(void);

/* A declared setting (API 17, setting_add). Copied: the strings need not stay. */
#define GN_SETTING_BOOL 1   /* a checkbox: 0 or 1 */
#define GN_SETTING_INT 2    /* a whole number (a slider when min < max) */
#define GN_SETTING_FLOAT 3  /* a number (a slider when min < max) */
#define GN_SETTING_CHOICE 4 /* one of `choices`: the value is its index */
#define GN_SETTING_TEXT 5   /* one line of text (def_text) */
#define GN_SETTING_HIDDEN 1 /* flags: only in the file, not on the settings page */
typedef struct gn_setting {
    uint32_t size;              /* sizeof(gn_setting) */
    int32_t kind;               /* GN_SETTING_* */
    const char* id;             /* its key in the mod's settings file */
    const char* label;          /* on the page (null: the id) */
    const char* description;    /* a tooltip, and a comment in the file (may be null) */
    const char* group;          /* settings with the same group are shown under one heading (may be null) */
    double def;                 /* default (BOOL 0 / 1, CHOICE an index) */
    double min;                 /* range (min == max: no range) */
    double max;
    double step;                /* INT / FLOAT without a range: the +/- buttons' step (0: none) */
    const char* const* choices; /* CHOICE: null-terminated labels */
    const char* def_text;       /* TEXT: the default */
    uint32_t flags;             /* GN_SETTING_HIDDEN */
} gn_setting;
typedef void (*gn_setting_fn)(const char* id, void* user);

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


/* The nearest thing a ray cast hit. Fixed: this layout never changes (gn_ray_hit_ex holds one, and
 * old mods pass it to raycast). New ray fields go at the end of gn_ray_hit_ex, which has a size. */
typedef struct gn_ray_hit {
    float position[3];
    float normal[3];   /* the surface's normal at the hit */
    float distance;    /* metres from the start of the ray */
    float fraction;    /* 0..1 along the ray */
} gn_ray_hit;

/* What a ray hit (raycast_object): the hit, and the body and entity. size: set it to
 * sizeof(gn_ray_hit_ex) before the call (later versions may add fields at the end). */
typedef struct gn_ray_hit_ex {
    uint32_t size;
    uint32_t body;        /* the collision body's id (entity_bodies lists an entity's); 0xffffffff: none */
    uint32_t part;        /* within the body: a static model group's prop, a compound's piece; 0xffffffff: none */
    uint32_t reserved;
    void* entity;         /* the server entity that owns the body, or null (level geometry, or not found) */
    void* physics_object; /* the game's object for the body (a RigidBody, or a group's body) */
    gn_ray_hit hit;
    /* API 19 (filled when size covers them): */
    uint32_t filter_info; /* the Havok body's collision filter info; 0 when not known (a group's body) */
    uint32_t layer;       /* its collision layer (filter_info & 31, RigidBodyCollisionLayer: 1 Static,
                           * 2 Dynamic, 3 Player, 4 AI, 14 Character, 21 CameraCollision, ...); 0: not known */
    uint32_t material;    /* the Havok body's material id */
    uint32_t reserved2;
} gn_ray_hit_ex;

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

/* An entity found by entities_of_class: it and its type. */
typedef void (*gn_entity_each_fn)(void* entity, const gn_type* type, void* user);
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
 * does the math, gn::ui::line3d and friends (gridnode/ui.hpp) draw with it. Frostbite is y up.
 * Later versions may add fields at the end: view_get fills only the fields below (API 10), and
 * view_get_ex (API 23) fills as many as both sides know, from the size the caller passes. */
typedef struct gn_view {
    float position[3]; /* the camera, world space */
    float forward[3];  /* the way it looks (unit) */
    float right[3];    /* unit */
    float up[3];       /* unit */
    float fov_y;       /* vertical field of view, degrees */
    float width;       /* the screen, pixels (the overlay's display size) */
    float height;
} gn_view;

/* ANT's types (ant_type_*, API 25): the animation and movement system's own reflection (assets,
 * drivers, processors, values), separate from Frostbite's types. size: set it to the struct's size
 * before the call; later versions may add fields at the end. Strings stay valid for the whole run. */
typedef struct gn_ant_type {
    uint32_t size;
    uint32_t id;              /* ANT's type id (the layout's CRC-32) */
    const char* name;
    uint32_t data_size;       /* the size of its data */
    uint32_t alignment;
    uint32_t field_count;     /* its own fields (ant_field_at); inherited ones are found by ant_field_find */
    uint32_t reserved;
    const void* layout;       /* the game's layout object (research) */
} gn_ant_type;
typedef struct gn_ant_field {
    uint32_t size;
    int32_t index;            /* its place in the type's declaration */
    const char* name;
    const char* type_name;    /* its type's ANT name ("" when not known) */
    uint32_t offset;          /* where it is in the type's data */
    int32_t instance_offset;  /* where it is in a live asset object (offset + the asset's header, measured
                               * by GridNode); -1 when not known */
    uint32_t element_size;
    uint32_t count;           /* array length (1: one value) */
    uint32_t flags;           /* 1: a reference (16 bytes); 2 / 4 / 8: aligned to at least 4 / 8 / 16 */
    uint32_t type_id;
} gn_ant_field;

/* Faith's move tracker (move_tracker, API 25): what ANT says about her move each client tick, through
 * the game's PamClientMoveTrackerComponent. size: set it before the call. */
typedef struct gn_move_tracker {
    uint32_t size;
    int32_t current_move;     /* the move she is in (ANT's CurrentMove; -1: none) */
    int32_t damage_type;      /* -1: none */
    int32_t in_jump_state;    /* 1 in a jump (ANT's IsInJumpState) */
    float moving_speed_xz;    /* horizontal speed as ANT has it, m/s */
    float moved_this_tick;    /* metres her position moved since the last update */
    float move_position[3];   /* ANT's MovePosition */
    float move_normal[3];     /* ANT's MoveNormal */
} gn_move_tracker;

/* Where an ANT type's field is in its live objects (API 26, ant_live_field), found by running the
 * type's own create and load on test data. size: set it to sizeof(gn_ant_live_field). */
typedef struct gn_ant_live_field {
    uint32_t size;
    int32_t offset;          /* in the live object; -1: not found (8-byte fields, or the loader does not copy it) */
    uint32_t bytes;          /* what the loader writes there (1 a bool, 4 a float or int; 8 a reference) */
    int32_t is_reference;    /* 1: a pointer to another asset at offset, not a value */
    uint32_t reference_type; /* a reference's type hash (as the loader passes it) */
    uint32_t reserved;
    void* vtable;            /* the type's objects start with this (ant_objects finds them by it) */
} gn_ant_live_field;

/* A camera a mod places (API 27, camera_set). */
#define GN_CAMERA_BODY 1 /* show Faith's whole body (as the free camera does), not the first-person arms */

/* Faith's recent movement as the game records it for time trials (API 27, replay_recent). */
typedef struct gn_replay_bone {
    float translation[4]; /* x, y, z, then a scale (1) */
    float rotation[4];    /* a quaternion x, y, z, w */
} gn_replay_bone;
typedef struct gn_replay_info {
    uint32_t size;        /* sizeof(gn_replay_info) */
    uint32_t frames;      /* keyframes held now (up to capacity) */
    uint32_t capacity;    /* the ring's length (450: 15 s at 30 a second) */
    uint32_t bones;       /* bones per keyframe */
    float newest_time;    /* the newest keyframe's time, seconds */
    uint32_t reserved;
} gn_replay_info;

/* An entity about to be destroyed (API 26, on_destroyed): still whole, on the thread destroying it.
 * Forget it here; do not keep it or call into the game with it. */
typedef void (*gn_destroyed_fn)(void* entity, void* user);
/* The level is ready (API 26, on_level_ready): Faith is over loaded ground. Server thread. */
typedef void (*gn_level_ready_fn)(const char* level, void* user);

/* An event an entity received (on_event), before it handles it: the entity, its type (null if unknown)
 * and the event's id (the djb2-xor hash of its name; gn::eventHash in mod.hpp). On the thread that
 * delivered it, usually the entity's realm thread (server or client); keep it short. */
typedef void (*gn_event_fn)(void* entity, const gn_type* type, uint32_t event_id, void* user);

/* GridNode's free camera (freecam_get): the one the layout editor flies. */
typedef struct gn_freecam {
    int32_t on;        /* flying now */
    float position[3]; /* where it is (world) */
} gn_freecam;

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

/* A progression flag (API 22). Strings and pointers stay valid while the level does. */
typedef struct gn_progress_flag {
    void* flag;             /* the game's PamProgressionFlag (for progress_set) */
    const char* name;       /* its Name */
    const char* group;      /* the flag group or mission it is in ("" if none) */
    uint32_t name_hash;     /* NameHash: the key the game stores the value by */
    uint32_t mission_index; /* the mission it belongs to (0: none) */
    int32_t value;          /* now (0 when the game holds no value for it) */
    int32_t max_value;      /* MaxValue (with clamp, values stay in 0 .. max_value) */
    uint8_t clamp;
    uint8_t online;         /* the data marks it as synced to online stats (many never reach a service); progress_set still sets it */
    uint8_t has_value;      /* the game holds a value for it */
    uint8_t reserved;
} gn_progress_flag;
typedef void (*gn_progress_flag_fn)(const gn_progress_flag* flag, void* user);

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
     * This changes a pointer, not code (for code: patch_call, patch_bytes). Hooks stay until the mod is
     * unloaded (or GridNode is). */
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
     * or null. Online console commands need online_allow (API 23); settings objects cannot be
     * checked (raw memory), so settings_find warns a mod that has not called it. */
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
     * the screen size, for drawing at world positions: a copy taken as the frame starts, so any
     * thread may ask (a UI callback gets this frame's). GN_ERR_UNAVAILABLE with no camera (loading,
     * menus) or no overlay. */
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

    /* ==== GN_API_VERSION 12 =================================================================== */

    /* ---- sending events --------------------------------------------------------------------------
     * Delivers an event to an entity as its bus would (its onEvent, with the event's id), on the
     * entity's realm thread at its next tick: the game acts on it as on one its own logic sent
     * ("StartReplay" to a PamServerReplayEntity spawns its ghost). event: a name or "0x<hash>".
     * event_send: one entity; GN_ERR_NOT_FOUND when it is not a live entity GridNode has seen.
     * event_send_class: every live entity of the class and its subclasses; how many, or -GN_ERR_*.
     * on_event listeners (and the Recorder) see it like any other. Any thread. */
    int (*event_send)(const char* mod, void* entity, const char* event);
    int (*event_send_class)(const char* mod, const char* class_name, const char* event);

    /* ==== GN_API_VERSION 13 =================================================================== */

    /* ---- the free camera -------------------------------------------------------------------------
     * GridNode's free camera (the layout editor's), for mods: the view leaves Faith and flies (mouse
     * looks, W A S D Q E fly, Shift / Ctrl faster / slower, wheel: speed); the game gets no input
     * while it flies with the menu closed, mods' hotkeys still work. freecam_set: on / off, at the
     * next frame (GN_ERR_UNAVAILABLE without Faith). teleport: Faith to where the free camera's
     * crosshair points (to = 1: on what it hits within 200 m, else the camera) or to the camera itself
     * (to = 2), facing the way it looks, at the next frame; keep_flying 0 goes back to her view.
     * GN_ERR_UNAVAILABLE while the camera is not flying. Any thread. */
    int (*freecam_set)(int on);
    int (*freecam_get)(gn_freecam* out);
    int (*freecam_teleport)(int to, int keep_flying);

    /* ==== GN_API_VERSION 14 =================================================================== */

    /* ---- entities of a class ---------------------------------------------------------------------
     * fn for every live entity of the class and its subclasses that GridNode saw created (logic
     * entities too: triggers, checkpoints, which have no place of their own and so never show in
     * entities_near). How many, or -GN_ERR_NOT_FOUND for an unknown class. Entities made before
     * GridNode loaded are not seen (inject it before the level loads). Any thread (it reads). */
    int (*entities_of_class)(const char* class_name, gn_entity_each_fn fn, void* user);

    /* ==== GN_API_VERSION 15 =================================================================== */

    /* ---- unloading -------------------------------------------------------------------------------
     * mod_unload: unloads the mod that calls it (found by the calling code's address), on GridNode's
     * thread once the current callback has returned: its gn_mod_unload runs, then GridNode takes out
     * everything it registered and frees it, as the GridNode menu's Unload does. It stays unloaded
     * until the menu's Load or a restart. GN_ERR_UNAVAILABLE for a mod without gn_mod_unload;
     * GN_ERR_NOT_FOUND when not called from a mod. For an "Unload" button in a mod's own window.
     * gridnode_unload: unloads every mod and GridNode itself, putting the game back as it was (the
     * menu's Unload section). Any thread; both return at once. */
    int (*mod_unload)(void);
    int (*gridnode_unload)(void);

    /* ---- code patches ----------------------------------------------------------------------------
     * For what no pointer reaches (a direct call); prefer hook_slot. Every patch is listed (the GridNode
     * menu's Hooks section) and undone when the mod that made it is unloaded, or GridNode is. Patches
     * may not overlap (GN_ERR_SLOT).
     * patch_bytes: `size` bytes (at most 64) at va become `bytes`, only if they are `expected` now
     * (GN_ERR_SLOT otherwise: another game build).
     * patch_call: the call or jmp rel32 at site_va, which must go to expected_target_va, goes to detour
     * instead (through a thunk GridNode puts near the image); *original receives the target: call
     * through it. GN_ERR_SLOT: no call or jmp there, or it goes elsewhere.
     * Any thread. The bytes change in one store when they fit in an aligned 8 or 16 bytes (a call's
     * rel32 nearly always does); longer ones are written with the game's other threads paused and
     * none about to run them. Either way a thread running the code sees the old or the new, never
     * half. */
    int (*patch_bytes)(const char* mod, uint64_t va, const void* expected, const void* bytes, size_t size);
    int (*patch_call)(const char* mod, uint64_t site_va, uint64_t expected_target_va, void* detour, void** original);

    /* ==== GN_API_VERSION 16 =================================================================== */

    /* ---- function detours ------------------------------------------------------------------------
     * Any function of the game, not only a virtual one: calls to function_va go to detour, which gets
     * the function's own arguments; *original receives what to call for the game's behaviour (call
     * it before, after, or not at all). The function's first instructions are moved to a trampoline
     * near the image and a jump put at its entry, once per function; several mods may detour the
     * same function (each gets the one below as its original, as with hook_slot). Taken out when the
     * mod is unloaded; the entry jump goes when GridNode is. GN_ERR_SLOT when the entry cannot be
     * moved (the function is shorter than 5 bytes, or starts with a jump back into itself);
     * GN_ERR_ARGUMENT when function_va is not in the game's code. Any thread. A detour runs on
     * whatever thread calls the function: keep it short and thread safe. */
    int (*hook_function)(const char* mod, uint64_t function_va, void* detour, void** original);

    /* ---- functions between mods ------------------------------------------------------------------
     * mod_export: offers fn (in the calling mod's code) to other mods as "<its id>.<name>".
     * mod_import: another mod's function, or null (not loaded, or not offered); call it from
     * gn_mod_load or later, after declaring that mod a dependency so it loads first. A mod that
     * imported from another keeps it loaded: that one cannot be unloaded on its own.
     * mod_version: GN_OK and its version (into out, may be null) when that mod is loaded,
     * GN_ERR_NOT_FOUND otherwise (an optional dependency's check). Any thread. */
    int (*mod_export)(const char* name, void* fn);
    void* (*mod_import)(const char* mod_id, const char* name);
    int (*mod_version)(const char* mod_id, char* out, size_t size);

    /* ==== GN_API_VERSION 17 =================================================================== */

    /* ---- declared settings -----------------------------------------------------------------------
     * A setting declared once with its kind, default, range and description. GridNode keeps it in the
     * mod's settings file (the same file as config_get / config_set, its key the setting's id, with
     * its label and description as a comment above it), clamps what is set to the range, and draws
     * it on the mod's page in the GridNode menu (Mod settings), which every mod with declared settings
     * gets unless it hides it (settings_page). Without GridNode's menu the file is the way to change
     * them. The same id again replaces the declaration (a reload).
     * setting_number: the value of a BOOL, INT, FLOAT or CHOICE (the index); NaN for an unknown id or
     * a TEXT. setting_text: any setting as text (out cut to fit: GN_ERR_FULL). setting_set_*: clamped
     * to the range, and the mod's listeners told (setting_on_change: fn(id, user) on the client
     * thread; id null: any of the mod's settings). Declarations and listeners go when the mod is
     * unloaded. Any thread. */
    int (*setting_add)(const char* mod, const gn_setting* setting);
    double (*setting_number)(const char* mod, const char* id);
    int (*setting_text)(const char* mod, const char* id, char* out, size_t size);
    int (*setting_set_number)(const char* mod, const char* id, double value);
    int (*setting_set_text)(const char* mod, const char* id, const char* value);
    int (*setting_on_change)(const char* mod, const char* id, gn_setting_fn fn, void* user);
    int (*settings_page)(const char* mod, int shown); /* shown 0: no page for this mod (its own window has them) */

    /* ==== GN_API_VERSION 18 =================================================================== */

    /* ---- what a ray hit, collision bodies ------------------------------------------------------
     * raycast_object: like raycast (a thin ray, no radius), and says what it hit: the body, the
     * part within it and the server entity that owns it (out->size must be set). Finding the
     * entity walks the server's physics entities (about a millisecond in the city): fine per
     * click, not every tick. Server thread. GN_ERR_NOT_FOUND: nothing hit; GN_ERR_UNAVAILABLE: no
     * level (or the caster is not as expected).
     * entity_bodies: the ids of a physics entity's collision bodies (ServerPhysicsEntity or
     * ClientPhysicsEntity, e.g. a prop) into ids (up to max; may be null with max 0); returns how
     * many it has (at most 64), or a negative error (-GN_ERR_ARGUMENT: not a physics entity, or
     * destroyed). Any thread (it reads).
     * body_transform / body_set_transform: one of the entity's bodies, where its collision is (a
     * body keeps its own place: moving the entity with entity_set_transform does not move it).
     * Set moves it as the game teleports a body (no sweep, no velocity), rotation rows unit
     * length (bodies have no scale). The entity's realm thread (Server... on the server thread,
     * Client... on the client thread): GN_ERR_THREAD otherwise. GN_ERR_NOT_FOUND: not one of its
     * bodies; GN_ERR_UNAVAILABLE: the body is not a RigidBody (a static model group's shared body).
     * Each realm has its own physics: Faith (on the client) walks into the client half's bodies, AI
     * and the server's ray casts hit the server half's. Move both to move what she collides with.
     * entity_other_half: the same object in the other realm (ServerX <-> ClientX at the same
     * place, within 5 cm), or null. Any thread (it reads; looks through that class). */
    int (*raycast_object)(const float from[3], const float to[3], gn_ray_hit_ex* out);
    int (*entity_bodies)(void* entity, uint32_t* ids, int max);
    int (*body_transform)(void* entity, uint32_t body, gn_transform* out);
    int (*body_set_transform)(void* entity, uint32_t body, const gn_transform* to);
    void* (*entity_other_half)(void* entity);

    /* ==== GN_API_VERSION 19 =================================================================== */

    /* ---- collision boxes -------------------------------------------------------------------------
     * Static boxes in the client's physics world (what Faith collides with) and the server's (AI, the
     * server's ray casts), the way the cinematic
     * tools make their street floors. Each box borrows a live static body's identity (so hits on it
     * report that body's material and owner); it is made, moved and destroyed at the client world's
     * next physics update, and made again in a new level's world. Nothing is made while the world
     * has fewer than 4,096 places left. Any thread.
     * box_add: half_extents in metres (0.01 .. 1000), at = its centre and rotation (rows unit length).
     * box_state: 1 in the world, 0 waiting (no level, or no static body to borrow from yet),
     * -GN_ERR_FULL Havok did not make it, -GN_ERR_NOT_FOUND unknown (the client world's box: the
     * server's follows at its own update). A mod's boxes go when it is unloaded. */
    int (*box_add)(const char* mod, const gn_transform* at, const float half_extents[3], uint32_t* out_box);
    int (*box_move)(uint32_t box, const gn_transform* at);
    int (*box_remove)(uint32_t box);
    int (*box_state)(uint32_t box);

    /* ---- sounds ------------------------------------------------------------------------------------
     * sound_play: a voice of the game's sound system for a loaded sound asset (asset_find's path,
     * e.g. a SoundPatchAsset), placed at pos (null: wherever the asset puts it), sent event (null:
     * the asset's DefaultStartEvent). sound_event sends another event (a name: "Start", "Stop", ...
     * as the asset's graph names them); sound_move places it again; sound_playing: 1 / 0;
     * sound_stop lets it go. A mod's sounds go when it is unloaded. Client thread (GN_ERR_THREAD
     * elsewhere; sound_playing: any). */
    int (*sound_play)(const char* mod, const char* asset_path, const char* event, const float pos[3], uint32_t* out_sound);
    int (*sound_event)(uint32_t sound, const char* event);
    int (*sound_move)(uint32_t sound, const float pos[3]);
    int (*sound_playing)(uint32_t sound);
    int (*sound_stop)(uint32_t sound);

    /* ---- despawning characters ----------------------------------------------------------------------
     * character_despawn: asks the game to take a server character away (an AI: a guard, a runner),
     * the way its spawner's "Unspawn" and the AI's own despawn do; it goes at the game's next
     * removal pass. Not Faith (GN_ERR_ARGUMENT). Server thread. */
    int (*character_despawn)(void* server_character);

    /* ==== GN_API_VERSION 20 =================================================================== */

    /* mod_dir: the folder of the calling mod's DLL as installed (mods\ or mods\<name>\, ending
     * with a backslash), for the files it brings: a mod runs from a copy (reloads), so its own
     * module path is not that. GN_ERR_FULL: out too small. Any thread. */
    int (*mod_dir)(char* out, size_t size);

    /* ==== GN_API_VERSION 21 =================================================================== */

    /* ---- body velocities (EXPERIMENTAL) ----------------------------------------------------------
     * Experimental: almost no body in the city is dynamic, so there is rarely anything to act on; may change.
     * A moving (dynamic) collision body's velocity, in m/s and rad/s, both in world space: props
     * that fall and roll, dropped weapons, ragdoll parts. body_set_velocity sets it (linear or
     * angular null: kept) and wakes the body; body_push adds to its linear velocity (an impulse
     * divided by its mass). A static body reads zeros and gives GN_ERR_UNAVAILABLE when set. Each
     * realm has its own physics (entity_other_half). The entity's realm thread. */
    int (*body_velocity)(void* entity, uint32_t body, float linear[3], float angular[3]);
    int (*body_set_velocity)(void* entity, uint32_t body, const float linear[3], const float angular[3]);
    int (*body_push)(void* entity, uint32_t body, const float change[3]);

    /* ---- gravity (EXPERIMENTAL) --------------------------------------------------------------------
     * Experimental: it moves dynamic bodies only, and there are few; may change.
     * A physics world's gravity (realm 0: the client's, what Faith's world falls by; 1: the server's),
     * m/s^2, (0, -9.81, 0) as the game makes it. It moves dynamic bodies (props, ragdolls, dropped
     * weapons); Faith's own movement is the game's character code, not this. physics_set_gravity
     * takes effect at the world's next physics update and is kept for later worlds (a level load);
     * g null puts the game's value back. One value per realm (the last call wins); undone when the
     * mod that set it unloads. Any thread. GN_ERR_UNAVAILABLE: no world yet (gravity: no level). */
    int (*physics_gravity)(int realm, float out[3]);
    int (*physics_set_gravity)(int realm, const float g[3]);

    /* ==== GN_API_VERSION 22 =================================================================== */

    /* ---- health ------------------------------------------------------------------------------------
     * A server character's health and max health, from its health component (Faith: 100 / 100 as
     * the game makes her; a KSEC: hits, 3 / 3). character_set_health writes the health only: the
     * game's damage path is not run, so 0 is not a death; the next hit acts on the new value.
     * Server thread. GN_ERR_ARGUMENT: not a server character; GN_ERR_UNAVAILABLE: no health
     * component. */
    int (*character_health)(void* server_character, float* health, float* max_health);
    int (*character_set_health)(void* server_character, float health);

    /* ---- where to go (EXPERIMENTAL: waypoint height, checkpoint source) ----------------------------
     * waypoint: the map marker the player set (world space; its height is the street's, so under a
     * building it is not where to stand); GN_ERR_NOT_FOUND when none is set.
     * checkpoint: where Faith comes back after a death. Any thread (they read).
     * GN_ERR_UNAVAILABLE: not found in this level or not as expected. */
    int (*waypoint)(float out[3]);
    int (*checkpoint)(float out[3]);

    /* ---- progression -------------------------------------------------------------------------------
     * The save's progression: every flag the game defines (PamProgressionData: flag groups,
     * missions' flags, the system's generated flags) with its value in the running game. What the
     * game saves is these values, so a change is in the next save the game makes.
     * progress_flags calls fn for each (names and pointers are valid during the call); returns how
     * many, negative on an error. progress_find: one flag by name (case sensitive).
     * progress_set sets one through the game's own setter (it clamps, sends
     * PamProgressionFlagUpdatedMessage, tells missions and the entities bound to the flag), and
     * returns GN_OK with *now the value the game kept. Flags marked as synced to online stats
     * (gn_progress_flag.online) are set too (since API 23; the log notes each one). progress_set: server thread; the others any thread (they read).
     * mission: the active mission's index (0: none) and, when name is given, its name. */
    int (*progress_flags)(gn_progress_flag_fn fn, void* user);
    int (*progress_find)(const char* name, gn_progress_flag* out);
    int (*progress_set)(void* flag, int32_t value, int32_t* now);
    int (*mission)(int32_t* index, char* name, size_t name_size);

    /* ==== GN_API_VERSION 23 =================================================================== */

    /* ---- online services, on purpose ---------------------------------------------------------------
     * online_allow: the mod says it changes online services on purpose (restoring a feature EA
     * turned off, pointing the game at a community server, ...); reason: one line saying what, shown
     * in the menu's Mods list and written to the log with a warning. Until a mod calls it,
     * console_run refuses online commands (GN_ERR_UNAVAILABLE; see online_warning); afterwards
     * they run, each one logged. Settings objects
     * are raw memory, so writes to them cannot be checked: settings_find warns (once) when a mod that
     * has not called this takes an online one. Taken back when the mod unloads. Any thread.
     * GN_ERR_ARGUMENT: no reason, or not called from a mod's code.
     * This is not for leaderboards: GridNode does not stop a mod that cheats online, but it makes
     * sure the player can see which mods touch online play. The MEC trainer never calls it (while it
     * is loaded, Dash and time trial uploads are blocked).
     * view_get_ex: view_get with the size of the caller's gn_view (sizeof(gn_view)), so gn_view can
     * grow at its end later. GN_ERR_ARGUMENT: size below the API 10 layout. */
    int (*online_allow)(const char* mod, const char* reason);
    int (*view_get_ex)(gn_view* out, uint32_t size);

    /* ==== GN_API_VERSION 24 =================================================================== */

    /* ---- the player's own input ---------------------------------------------------------------------
     * action_held: whether the player holds an input action now (by its hash, e.g. GN_ACTION_JUMP),
     * whatever key or button it is bound to: 1 held, 0 not, or a negative error (-GN_ERR_NOT_FOUND: no
     * such digital action; -GN_ERR_UNAVAILABLE: no Faith). It reads the player's real input, so
     * action_press (and jump) do not show in it. The game updates it once a frame; read it on the
     * client tick for timing. Any thread (it reads). */
    int (*action_held)(uint32_t action_hash);

    /* ==== GN_API_VERSION 25 =================================================================== */

    /* ---- ANT's types -----------------------------------------------------------------------------
     * ANT (the animation and movement system) registers every asset, driver, processor and value
     * type by name with a layout of its fields. ant_type_count: how many (negative: not read yet,
     * the game is still starting). ant_type_at / ant_type_find: one type by index (sorted by name)
     * or name. ant_field_at: a type's own fields by index (0..field_count-1); ant_field_find: a field
     * by name, in the type or its bases. To change a value in a live asset (an ANT object you have,
     * e.g. from a hook), use instance_offset (when not -1); a field with flags & 1 is a reference to
     * another asset, not a value. Any thread (they read; the first call reads the registry).
     * GN_ERR_UNAVAILABLE: not read yet; GN_ERR_NOT_FOUND: no such type, field or index. */
    int (*ant_type_count)(void);
    int (*ant_type_at)(uint32_t index, gn_ant_type* out);
    int (*ant_type_find)(const char* name, gn_ant_type* out);
    int (*ant_field_at)(const char* type_name, uint32_t index, gn_ant_field* out);
    int (*ant_field_find)(const char* type_name, const char* field_name, gn_ant_field* out);

    /* ---- Faith's move tracker ------------------------------------------------------------------------
     * move_tracker: her current move, whether she is in a jump, her speed as ANT has it (see
     * gn_move_tracker). Client thread (GN_ERR_THREAD otherwise). GN_ERR_UNAVAILABLE: no Faith, or
     * her tracker is not bound to ANT yet. */
    int (*move_tracker)(gn_move_tracker* out);

    /* ==== GN_API_VERSION 26 =================================================================== */

    /* ---- destroyed entities ----------------------------------------------------------------------
     * on_destroyed: fn(entity, user) for every entity of class_name (or a subclass) just before the
     * game destroys it, on the thread destroying it (any game thread; keep it short). A mod that keeps
     * entities (from on_created, entities_of_class, ...) drops them here instead of finding them dead.
     * GN_ERR_NOT_FOUND: no such class; GN_ERR_FULL: 256 listeners. Taken out when the mod unloads. */
    int (*on_destroyed)(const char* mod, const char* class_name, gn_destroyed_fn fn, void* user);

    /* ---- the level is ready -----------------------------------------------------------------------
     * on_level's "loaded" comes when the level's name is there, before its ground has streamed in.
     * on_level_ready: fn(level, user) once per level, on the server thread, when Faith has been over
     * loaded ground for about a second (a mod registered later is told at the next server tick).
     * level_ready: 1 when the current level is ready, else 0. Any thread.
     * ground_below: a ray from pos down `depth` metres (0: 100): GN_OK and *ground_y when something
     * is there (the place has streamed in), GN_ERR_NOT_FOUND when nothing is. Server thread
     * (GN_ERR_THREAD otherwise). */
    int (*on_level_ready)(const char* mod, gn_level_ready_fn fn, void* user);
    int (*level_ready)(void);
    int (*ground_below)(const float pos[3], float depth, float* ground_y);

    /* ---- ANT fields in live objects (research, EXPERIMENTAL) -----------------------------------------
     * Experimental: a research tool; may change.
     * ant_live_field: where field_name (the type's own fields, not its bases') is in type_name's live
     * objects (see gn_ant_live_field). The first call for a type runs its create and load on test data
     * (logged; a few ms). ant_objects: the type's live objects, found by its vtable in the game's
     * memory: up to max into out, returns how many there are (negative: an error). It scans when the
     * last scan's objects are gone, which can take seconds: call it once, keep them, drop them in
     * on_level. Objects of other types that share the vtable can be among them. Both: client thread
     * (GN_ERR_THREAD); GN_ERR_UNAVAILABLE: ANT not read yet, or the type cannot be mapped. Write
     * values with write(); a driver may only read a value when its move starts. */
    int (*ant_live_field)(const char* type_name, const char* field_name, gn_ant_live_field* out);
    int (*ant_objects)(const char* type_name, void** out, uint32_t max);

    /* ==== GN_API_VERSION 27 =================================================================== */

    /* ---- the camera -------------------------------------------------------------------------------
     * camera_set: the game's camera is at *t (right, up, forward, position; the view looks along
     * MINUS forward, as gn_view and the game have it) from the next frame on, until camera_set(null)
     * gives it back. Call it again each frame to move it. GN_CAMERA_BODY shows Faith's whole body.
     * GridNode's free camera, when on, comes first. camera_fov_set: the camera's vertical field of
     * view in degrees before the player's FOV setting (the game's is 55); 0 gives it back. Both: any
     * thread; given back when the mod unloads. GN_ERR_UNAVAILABLE: no Faith (menus, loading). */
    int (*camera_set)(const gn_transform* t, uint32_t flags);
    int (*camera_fov_set)(float degrees);

    /* ---- Faith's last 15 seconds --------------------------------------------------------------------
     * The game keeps a ring of her pose (450 keyframes, one a client tick) to make time trial
     * ghosts. Experimental: the bones past the root and the trajectory are not mapped; may change.
     * replay_recent fills *info and copies the newest min(max_frames, info->frames)
     * keyframes, oldest first: times[i] (seconds) and bones[i * info->bones + b]. times and bones may be
     * null (max_frames 0: info only). Returns how many it copied, negative on an error. Bone 0 is the
     * root and bone 1 her trajectory (where she is); the bones are as ANT's pose has them (research:
     * docs/research/replay-ghosts.md). Client thread (GN_ERR_THREAD); GN_ERR_UNAVAILABLE: no Faith,
     * or her recorder has not started. */
    int (*replay_recent)(gn_replay_info* info, float* times, gn_replay_bone* bones, uint32_t max_frames);

    /* ---- the menu -----------------------------------------------------------------------------------
     * ui_menu_set: opens (1) or closes (0) the overlay's menu, as its key does: a mod with its own key
     * (a hotkey) shows its window that way. With [ui] menu_key=NONE in gridnode.ini only mods open it,
     * and gridnode_window=0 leaves the GridNode window out (a mod shipped with GridNode on its own).
     * Any thread. GN_ERR_UNAVAILABLE: no overlay. */
    int (*ui_menu_set)(int open);

    /* ==== GN_API_VERSION 28 =================================================================== */

    /* ---- holding the time of day ---------------------------------------------------------------------
     * time_of_day_hold: the clock stays at `seconds` (0 .. 86400) until released: it is written back
     * after each server and each client world update, so the lighting never sees the clock the game
     * moved on (a single time_of_day_set is advanced again by the next update, and the lighting follows
     * that drift). seconds < 0 releases it: the clock runs on from where it stands. One hold at a time
     * (the last call wins); released when the mod that set it unloads. Setting another time while held
     * is another call. Any thread. GN_ERR_UNAVAILABLE: no level yet (a hold set then waits for the
     * clock); GN_ERR_ARGUMENT: not a number. */
    int (*time_of_day_hold)(float seconds);

    /* ==== GN_API_VERSION 29 =================================================================== */

    /* ---- the visual environment (experimental) -------------------------------------------------------
     * The game's current visual environment state: sun, tonemap, colour correction, sky, wind, depth of
     * field, vignette, film grain. Offsets are within the state (GN_VE_* below, from the cinematic
     * tools; the fields inside each block are in docs/research/visual-environment.md). The game
     * recomputes the state every frame, so a value set here is a hold: it is written back after each
     * server and each client world update until released. At most 64 holds; one per (offset, size);
     * the last call wins. All are released when the mod that set them unloads. Experimental: whether a
     * held value is steady on screen (not flickering) is untested.
     * visual_env_get reads the state now; visual_env_set holds `size` bytes at `offset`;
     * visual_env_release drops the hold at `offset` (GN_VE_ALL: every hold, whoever set it). offset + size must
     * be within GN_VE_SIZE. Any thread. GN_ERR_UNAVAILABLE: no level yet (a hold set then waits);
     * GN_ERR_ARGUMENT: out of range, or too many holds. */
    int (*visual_env_get)(uint32_t offset, void* out, uint32_t size);
    int (*visual_env_set)(uint32_t offset, const void* data, uint32_t size);
    int (*visual_env_release)(uint32_t offset);

    /* ---- timelines (experimental) --------------------------------------------------------------------
     * Doors, gates and lifts are driven by TimelineEntity objects (find one with entities_near or
     * entities_of_class). timeline_time reads the current time (seconds); timeline_set_time sets it, and
     * with force != 0 what it drives is updated at once, in the same frame. The entity must be a
     * TimelineEntity (checked by type, GN_ERR_ARGUMENT). timeline_set_time: server thread (GN_ERR_THREAD).
     * Untested in game. */
    int (*timeline_time)(const void* timeline, float* seconds);
    int (*timeline_set_time)(void* timeline, float seconds, int force);

    /* ==== GN_API_VERSION 30 =================================================================== */

    /* ---- streaming focus points (experimental) -------------------------------------------------------
     * The city is zone-streamed around its focus points (Faith, normally), so far from her, as with the
     * free camera or after a far teleport, the world around you is not loaded. A focus point makes the
     * streamers load the zones around `pos` as well. streaming_focus_add makes one and gives its
     * handle; streaming_focus_move moves it; streaming_focus_remove drops it. At most 4 at a time
     * (GN_ERR_FULL), shared with GridNode's own (the layout editor's); points within 60 m of another count
     * as one. A point is registered and moved within a quarter second, not at once, and a zone loads as
     * the game loads zones (the districts of the city, not a circle). Released when the mod unloads.
     * Any thread. GN_ERR_UNAVAILABLE: this build's streamer code is not the known one; GN_ERR_NOT_FOUND:
     * an unknown handle; GN_ERR_ARGUMENT: not finite numbers. Untested in game. */
    int (*streaming_focus_add)(const float pos[3], uint32_t* handle);
    int (*streaming_focus_move)(uint32_t handle, const float pos[3]);
    int (*streaming_focus_remove)(uint32_t handle);
} gn_api;

typedef int (*gn_mod_load_fn)(const gn_api* api);

#ifdef __cplusplus
}
#endif

#endif /* GRIDNODE_API_H */
