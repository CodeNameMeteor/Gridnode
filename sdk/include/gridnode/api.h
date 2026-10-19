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

#define GN_API_VERSION 1

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

typedef struct gn_type gn_type; /* a runtime TypeInfo / ClassInfo of the engine */

typedef void (*gn_tick_fn)(float dt, void* user);
typedef void (*gn_task_fn)(void* user);
typedef void (*gn_created_fn)(void* entity, void* creation_info, void* user);

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
} gn_api;

typedef int (*gn_mod_load_fn)(const gn_api* api);

#ifdef __cplusplus
}
#endif

#endif /* GRIDNODE_API_H */
