// The Recorder in a player build (GRIDNODE_PLAYER_BUILD): not built, nothing recorded.
#include "host.hpp"

namespace gn::host::recorder {
void start(const std::wstring&) {}
void onLevel(const std::string&) {}
void draw(bool*) {}
void frame() {}
} // namespace gn::host::recorder
