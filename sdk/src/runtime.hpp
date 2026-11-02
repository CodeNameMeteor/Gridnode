#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace gn::runtime {

// GridNode's own thread: waits for the engine, checks the build, writes the type dump. Returns after
// a full unload (or when GridNode stays passive); true when gridnode.dll can be freed then.
bool run(HMODULE self);
// Process exit.
void shutdown();

} // namespace gn::runtime
