// lab.hpp: claim-lab experiments register themselves here (one file per research topic).
#pragma once

#include <vector>

#include "gridnode/mod.hpp"

namespace lab {

// An experiment tests the claims of one research topic in game, using only the GridNode API that
// exists today (gn::read, gn::function, gn::hookFunction, gn::patchBytes, ...). Nothing runs until
// the user ticks "Install" in the Claim Lab window, so a bad experiment cannot hurt the game at load.
struct Experiment {
    const char* topic;  // the research topic slug
    const char* claims; // one line: which claims this tests (ids from verified.md)
    bool (*install)();  // hooks and patches, once; false = failed (say why with gn::log)
    void (*draw)();     // ImGui body while installed and the menu is open (null: none)
    void (*reset)();    // zero counters / clear results (null: none)
};

std::vector<Experiment>& registry();

struct Registrar {
    explicit Registrar(const Experiment& e) { registry().push_back(e); }
};

} // namespace lab

// LAB_EXPERIMENT(unique_id, "topic", "claims tested", &install, &draw, &reset);
#define LAB_EXPERIMENT(ID, ...)                                  \
    namespace {                                                  \
    const lab::Registrar kLabRegistrar_##ID{lab::Experiment{__VA_ARGS__}}; \
    }
