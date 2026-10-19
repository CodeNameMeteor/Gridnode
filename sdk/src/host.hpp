#pragma once
// Internal state of the GridNode runtime inside the game (not part of the public API).

#include <atomic>
#include <cstdint>
#include <string>

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

} // namespace gn::host
