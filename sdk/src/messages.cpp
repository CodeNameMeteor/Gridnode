// The engine's message system (db/annotations/messages.yaml).
//
// GridNode registers its own listener objects with the engine's three message managers (core,
// client, server) through MessageManager_registerListener, the way the game's code does. Nothing
// is hooked or patched. A listener is registered per category (djb2-xor of the category name);
// GridNode then hands each message to the mods that asked for its category or its type.
//
// Categories are known from the message types' static instances (Message +0x08), type ids are the
// djb2-xor of the message class name (Message +0x0c). The managers can be recreated (the server
// game context when a server starts), so registrations are checked every tick of their realm.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gridnode/build.hpp"
#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

namespace e = gn::engine;

uint32_t nameHash(const std::string& s) {
    uint32_t h = 5381;
    for (unsigned char c : s) h = (h * 33) ^ c;
    return h;
}

// ---- what the engine's message types are -------------------------------------------------------
struct Catalogue {
    std::unordered_map<uint32_t, const reflect::Type*> byTypeId;  // djb2(name) -> message type
    std::unordered_map<uint32_t, uint32_t> categoryOfType;        // type id -> category
    std::map<uint32_t, std::string> categoryName;                 // category -> name
    std::vector<MessageType> types;                               // sorted by name
};

const Catalogue& catalogue() {
    static const Catalogue c = [] {
        Catalogue k;
        for (const reflect::Type& t : registry().data().types) {
            if (t.name.empty()) continue;
            if (t.category != reflect::Category::ValueType || !t.staticInstance) continue;
            uint32_t head[2] = {}; // category, type id
            if (!mem::read(static_cast<uintptr_t>(t.staticInstance) + e::st::Message::off::category, head, sizeof(head))) continue;
            if (head[1] != nameHash(t.name)) continue; // not a message (or not initialised)
            k.byTypeId[head[1]] = &t;
            k.categoryOfType[head[1]] = head[0];
            // The category's name is a prefix of the class name (ClientPlayer -> ClientPlayerLocalSetMessage).
            if (!k.categoryName.count(head[0]))
                for (size_t n = 1; n <= t.name.size(); ++n)
                    if (nameHash(t.name.substr(0, n)) == head[0]) {
                        k.categoryName[head[0]] = t.name.substr(0, n);
                        break;
                    }
            k.types.push_back({&t, head[1], head[0]});
        }
        std::sort(k.types.begin(), k.types.end(), [](const MessageType& a, const MessageType& b) { return a.type->name < b.type->name; });
        for (MessageType& m : k.types) {
            auto it = k.categoryName.find(m.category);
            m.categoryName = it != k.categoryName.end() ? it->second.c_str() : "?";
            m.online = onlineWarning(m.categoryName);
        }
        GN_LOG("messages: %llu message types in %llu categories", static_cast<unsigned long long>(k.types.size()),
               static_cast<unsigned long long>(k.categoryName.size()));
        return k;
    }();
    return c;
}

// ---- subscriptions ----------------------------------------------------------------------------
struct Sub {
    std::string mod;
    uint32_t typeId; // 0: the whole category
    gn_message_fn fn;
    void* user;
};
std::shared_mutex g_subM;
std::unordered_map<uint32_t, std::vector<Sub>> g_subs; // category -> subscribers (g_subM)
std::set<uint32_t> g_wanted;                            // categories to listen to (g_subM)
std::atomic<bool> g_monitor{false};
std::atomic<MessageRecorder> g_recorder{nullptr};
std::atomic<MessageRecorder> g_tap{nullptr}; // the recorder (recorder.cpp), independent of the inspector's

// ---- the listener objects ------------------------------------------------------------------------
// One per manager. The engine calls vtable[0](this, message) and vtable[1](this) for the order.
struct Listener {
    const void* const* vtable;
    int manager; // 0 core, 1 client, 2 server
};

void dispatch(int manager, const uint8_t* msg);

void onMessage(Listener* self, const uint8_t* msg) { dispatch(self->manager, msg); }
uint32_t listenerOrder(Listener*) { return 100; } // the engine's default
const void* const kVtable[2] = {reinterpret_cast<const void*>(&onMessage), reinterpret_cast<const void*>(&listenerOrder)};
Listener g_listeners[3] = {{kVtable, 0}, {kVtable, 1}, {kVtable, 2}};

void dispatch(int manager, const uint8_t* msg) {
    uint32_t head[3] = {}; // category, type id, realm
    memcpy(head, msg + e::st::Message::off::category, sizeof(head));
    if (MessageRecorder rec = g_recorder.load(std::memory_order_relaxed); rec && g_monitor.load(std::memory_order_relaxed))
        rec(manager, msg, head[0], head[1], head[2]);
    if (MessageRecorder tap = g_tap.load(std::memory_order_relaxed)) tap(manager, msg, head[0], head[1], head[2]);
    std::vector<Sub> subs;
    {
        std::shared_lock<std::shared_mutex> lock(g_subM);
        auto it = g_subs.find(head[0]);
        if (it == g_subs.end()) return;
        subs = it->second;
    }
    const Catalogue& c = catalogue();
    auto t = c.byTypeId.find(head[1]);
    auto cn = c.categoryName.find(head[0]);
    gn_message_info info{};
    info.type = reinterpret_cast<const gn_type*>(t != c.byTypeId.end() ? t->second : nullptr);
    info.type_name = t != c.byTypeId.end() ? t->second->name.c_str() : "";
    info.category = cn != c.categoryName.end() ? cn->second.c_str() : "";
    info.type_id = head[1];
    info.category_id = head[0];
    info.realm = head[2];
    info.manager = manager;
    for (const Sub& s : subs)
        if (!s.typeId || s.typeId == head[1]) s.fn(msg, &info, s.user);
}

// ---- registration with the managers --------------------------------------------------------------
// The MessageManager handle of each manager (null when it does not exist right now).
uintptr_t managerHandle(int manager) {
    switch (manager) {
    case 0: return mem::readOr<uintptr_t>(mem::rva(e::global::CoreMessageManager));
    case 1: {
        const uintptr_t ctx = mem::readOr<uintptr_t>(mem::rva(e::global::ClientGameContext));
        return ctx ? ctx + e::st::ClientGameContext::off::messageManager : 0;
    }
    default: {
        const uintptr_t ctx = mem::readOr<uintptr_t>(mem::rva(e::global::ServerGameContext));
        return ctx ? ctx + e::st::ServerGameContext::off::messageManager : 0;
    }
    }
}

struct Registered {
    uintptr_t impl = 0;           // the implementation the categories below were registered with
    std::set<uint32_t> categories;
};
Registered g_registered[3]; // each touched only by the thread that syncs it

bool available() {
    static const int ok = [] {
        const bool all = build::codeMatches("MessageManager_registerListener") && build::codeMatches("MessageManager_unregisterListener");
        if (!all) GN_LOG("messages: the message manager functions do not look as expected; messages are off");
        return all ? 1 : 0;
    }();
    return ok == 1;
}

// On the realm's thread: register the wanted categories with this manager (again, if it was recreated).
void sync(int manager) {
    if (!available()) return;
    const uintptr_t handle = managerHandle(manager);
    const uintptr_t impl = handle ? mem::readOr<uintptr_t>(handle) : 0;
    Registered& r = g_registered[manager];
    if (impl != r.impl) {
        r.impl = impl;
        r.categories.clear();
    }
    if (!impl) return;
    std::vector<uint32_t> todo;
    {
        std::shared_lock<std::shared_mutex> lock(g_subM);
        for (uint32_t c : g_wanted)
            if (!r.categories.count(c)) todo.push_back(c);
    }
    if (todo.empty()) return;
    using RegisterFn = uint8_t (*)(void* manager, uint32_t category, void* listener, uint32_t realm);
    const auto reg = reinterpret_cast<RegisterFn>(mem::rva(e::function::MessageManager_registerListener));
    for (uint32_t c : todo) {
        reg(reinterpret_cast<void*>(handle), c, &g_listeners[manager], 0xff);
        r.categories.insert(c);
    }
    static const char* const kNames[3] = {"core", "client", "server"};
    GN_LOG("messages: listening to %llu more categor%s on the %s message manager (%llu in all)",
           static_cast<unsigned long long>(todo.size()), todo.size() == 1 ? "y" : "ies", kNames[manager],
           static_cast<unsigned long long>(r.categories.size()));
}

void want(uint32_t category) {
    std::unique_lock<std::shared_mutex> lock(g_subM);
    g_wanted.insert(category);
}

} // namespace

// Ticks (ticks.cpp): keep the registrations in place. Core and client on the client thread.
void messagesClientTick() {
    sync(0);
    sync(1);
}
void messagesServerTick() { sync(2); }

const std::vector<MessageType>& messageTypes() { return catalogue().types; }

std::string messageCategoryName(uint32_t category) {
    const auto& n = catalogue().categoryName;
    auto it = n.find(category);
    return it != n.end() ? it->second : std::string();
}

const reflect::Type* messageTypeById(uint32_t typeId) {
    const auto& m = catalogue().byTypeId;
    auto it = m.find(typeId);
    return it != m.end() ? it->second : nullptr;
}

int onMessage(const std::string& mod, const std::string& name, gn_message_fn fn, void* user) {
    if (!fn || name.empty()) return GN_ERR_ARGUMENT;
    if (!available()) return GN_ERR_UNAVAILABLE;
    const Catalogue& c = catalogue();
    uint32_t category = 0, typeId = 0;
    const uint32_t h = nameHash(name);
    if (c.byTypeId.count(h)) { // a message type
        typeId = h;
        category = c.categoryOfType.at(h);
    } else if (c.categoryName.count(h)) { // a category
        category = h;
    } else {
        GN_LOG("[%s] on_message: no message type or category called %s", mod.c_str(), name.c_str());
        return GN_ERR_NOT_FOUND;
    }
    {
        std::unique_lock<std::shared_mutex> lock(g_subM);
        g_subs[category].push_back({mod, typeId, fn, user});
        g_wanted.insert(category);
    }
    GN_LOG("[%s] on_message(%s): %s %s", mod.c_str(), name.c_str(), typeId ? "message type in category" : "category",
           messageCategoryName(category).c_str());
    return GN_OK;
}

void messageMonitor(bool on, MessageRecorder rec) {
    if (rec) g_recorder = rec;
    if (on && available())
        for (const auto& kv : catalogue().categoryName) want(kv.first); // every category, from the next tick
    g_monitor = on;
}

void messageTap(MessageRecorder rec) {
    if (rec && available())
        for (const auto& kv : catalogue().categoryName) want(kv.first); // every category, from the next tick
    g_tap = rec;
}

bool messagesAvailable() { return available(); }

void forgetMessages(const CodeRange& r) {
    std::unique_lock<std::shared_mutex> lock(g_subM);
    for (auto& [category, subs] : g_subs)
        subs.erase(std::remove_if(subs.begin(), subs.end(), [&](const Sub& s) { return r.has(reinterpret_cast<void*>(s.fn)); }), subs.end());
    // The categories stay registered with the managers: GridNode's listener then finds nobody.
}

} // namespace gn::host
