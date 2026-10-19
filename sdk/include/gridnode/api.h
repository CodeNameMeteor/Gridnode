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

#define GN_API_VERSION 4 /* 2: world, levels and UI (M4); 3: settings and console (M4.5); 4: messages (M5) */

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
} gn_api;

typedef int (*gn_mod_load_fn)(const gn_api* api);

#ifdef __cplusplus
}
#endif

#endif /* GRIDNODE_API_H */
