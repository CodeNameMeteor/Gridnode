#pragma once
// C++ helpers for GridNode mods, header-only, over the C API in gridnode/api.h.
//
//   #include <gridnode/mod.hpp>
//
//   GN_MOD("my-mod") {          // runs once, on GridNode's thread, after the build check
//       gn::log("hello from %s", "my mod");
//       gn::onServerTick([](float dt) { ... });
//       return true;            // false: the mod failed to load
//   }
//
// Callbacks are std::function-like lambdas; GridNode keeps them for the life of the process.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

#include "gridnode/api.h"

namespace gn {

namespace detail {
inline const gn_api*& apiPtr() {
    static const gn_api* a = nullptr;
    return a;
}
inline const char*& modName() {
    static const char* n = "mod";
    return n;
}
template <class F> F* keep(F f) { return new F(std::move(f)); } // lives for the process, by design
} // namespace detail

inline const gn_api& api() { return *detail::apiPtr(); }
inline const char* modName() { return detail::modName(); }

// ---- logging ---------------------------------------------------------------------------------
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline void log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    api().log(modName(), buf);
}

// ---- addresses -------------------------------------------------------------------------------
inline uint64_t rva(uint64_t va) { return api().rva(va); }
template <class Fn> Fn function(uint64_t va) { return reinterpret_cast<Fn>(static_cast<uintptr_t>(rva(va))); }

// ---- threads ---------------------------------------------------------------------------------
template <class F> bool onServerTick(F f) {
    auto* p = detail::keep(std::function<void(float)>(std::move(f)));
    return api().on_server_tick([](float dt, void* u) { (*static_cast<std::function<void(float)>*>(u))(dt); }, p) == GN_OK;
}
template <class F> bool onClientTick(F f) {
    auto* p = detail::keep(std::function<void(float)>(std::move(f)));
    return api().on_client_tick([](float dt, void* u) { (*static_cast<std::function<void(float)>*>(u))(dt); }, p) == GN_OK;
}
template <class F> void runOnServer(F f) {
    auto* p = new std::function<void()>(std::move(f));
    api().run_on_server([](void* u) {
        auto* fn = static_cast<std::function<void()>*>(u);
        (*fn)();
        delete fn;
    }, p);
}
template <class F> void runOnClient(F f) {
    auto* p = new std::function<void()>(std::move(f));
    api().run_on_client([](void* u) {
        auto* fn = static_cast<std::function<void()>*>(u);
        (*fn)();
        delete fn;
    }, p);
}
inline bool isServerThread() { return api().is_server_thread() != 0; }
inline bool isClientThread() { return api().is_client_thread() != 0; }

// ---- hooks -----------------------------------------------------------------------------------
// Swaps the pointer at slotVa for detour; *original receives what it held (call through it).
// expectedVa: the game function the slot must hold (0: any game function).
template <class Fn> bool hookSlot(uint64_t slotVa, uint64_t expectedVa, Fn detour, Fn* original) {
    static_assert(std::is_pointer<Fn>::value, "detour must be a function pointer");
    return api().hook_slot(modName(), slotVa, expectedVa, reinterpret_cast<void*>(detour),
                           reinterpret_cast<void**>(original)) == GN_OK;
}

// ---- reflection ------------------------------------------------------------------------------
class Type {
public:
    Type() = default;
    explicit Type(const gn_type* t) : t_(t) {}
    static Type find(const char* name) { return Type(api().type_find(name)); }
    static Type of(const void* object) { return Type(api().type_of(object)); }

    explicit operator bool() const { return t_ != nullptr; }
    const gn_type* get() const { return t_; }
    const char* name() const { return t_ ? api().type_name(t_) : ""; }
    uint32_t size() const { return t_ ? api().type_size(t_) : 0; }
    int category() const { return api().type_category(t_); }
    Type super() const { return Type(t_ ? api().type_super(t_) : nullptr); }
    uint64_t va() const { return t_ ? api().type_va(t_) : 0; }
    bool isKindOf(const Type& base) const { return t_ && base.t_ && api().is_kind_of(t_, base.t_) != 0; }
    bool field(const char* name, gn_field& out) const { return t_ && api().field_find(t_, name, &out) == GN_OK; }
    uint32_t fieldCount() const { return t_ ? api().field_count(t_) : 0; }
    bool fieldAt(uint32_t i, gn_field& out) const { return t_ && api().field_at(t_, i, &out) == GN_OK; }
    bool operator==(const Type& o) const { return t_ == o.t_; }

private:
    const gn_type* t_ = nullptr;
};

// An engine object with its reflected type, for reading and writing fields by name. Reads and
// writes are fault tolerant and check the field's size against T.
class Object {
public:
    Object() = default;
    Object(void* p, Type t) : p_(p), t_(t) {}
    static Object of(void* p) { return Object(p, Type::of(p)); }

    explicit operator bool() const { return p_ && t_; }
    void* ptr() const { return p_; }
    const Type& type() const { return t_; }

    template <class T> bool get(const char* field, T& out) const {
        static_assert(std::is_trivially_copyable<T>::value, "fields are read as plain bytes");
        gn_field f;
        if (!t_.field(field, f) || f.size != sizeof(T)) return false;
        return api().read(static_cast<const char*>(p_) + f.offset, &out, sizeof(T)) == GN_OK;
    }
    template <class T> bool set(const char* field, const T& value) const {
        static_assert(std::is_trivially_copyable<T>::value, "fields are written as plain bytes");
        gn_field f;
        if (!t_.field(field, f) || f.size != sizeof(T)) return false;
        return api().write(static_cast<char*>(p_) + f.offset, &value, sizeof(T)) == GN_OK;
    }
    // A field that refers to another object (a class field: a pointer).
    Object ref(const char* field) const {
        void* p = nullptr;
        return get(field, p) && p ? Object::of(p) : Object();
    }

private:
    void* p_ = nullptr;
    Type t_;
};

template <class T> bool read(const void* address, T& out) { return api().read(address, &out, sizeof(T)) == GN_OK; }
template <class T> bool write(void* address, const T& value) { return api().write(address, &value, sizeof(T)) == GN_OK; }

// ---- entities --------------------------------------------------------------------------------
// fn(void* entity, void* creationInfo) for every entity of className (or a subclass) created from
// now on, on the thread that created it.
template <class F> bool onCreated(const char* className, F f) {
    auto* p = detail::keep(std::function<void(void*, void*)>(std::move(f)));
    return api().on_created(modName(), className,
                            [](void* e, void* ci, void* u) { (*static_cast<std::function<void(void*, void*)>*>(u))(e, ci); },
                            p) == GN_OK;
}

} // namespace gn

// Defines the mod's entry point. The body runs once and returns true when the mod loaded.
#define GN_MOD(NAME)                                                                              \
    static bool gn_mod_main();                                                                    \
    extern "C" GN_MOD_EXPORT int gn_mod_load(const gn_api* api) {                                 \
        if (!api || api->version < GN_API_VERSION || api->size < sizeof(gn_api)) return 1;         \
        ::gn::detail::apiPtr() = api;                                                             \
        ::gn::detail::modName() = NAME;                                                           \
        return gn_mod_main() ? 0 : 1;                                                             \
    }                                                                                             \
    static bool gn_mod_main()
