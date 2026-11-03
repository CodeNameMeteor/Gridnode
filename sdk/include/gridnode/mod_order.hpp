#pragma once
// Mod manifests: dependencies, versions and the load order worked out from them (mods.cpp).
// Portable: tested on the host (tests/runtime_core_test.cpp).

#include <string>
#include <vector>

namespace gn::modorder {

struct Dep {
    std::string id;
    std::string minVersion; // "" = any
    bool soft = false;      // "?id": load after it when it is there; not needed
};

// "id", "id>=1.2", "?id", "?id>=1.2" (spaces around the parts are ignored).
Dep parseDep(const std::string& text);

// Dotted numbers, missing parts 0 ("1.2" == "1.2.0"); a part's leading digits only ("2-beta" is 2).
// <0, 0, >0 as a is older, the same, newer.
int compareVersions(const std::string& a, const std::string& b);

struct Entry {
    std::string id;
    std::string version;
    std::vector<Dep> deps;
};

struct Plan {
    std::vector<size_t> order;         // indexes of the mods to load, dependencies first
    std::vector<std::string> problem;  // per mod: why it is not loaded ("" = it is in order)
};

// Dependencies first, otherwise the order given (file name order). A mod is left out, with the reason,
// when a hard dependency is missing, too old or itself left out, its id is taken by an earlier file,
// or it is in a dependency cycle.
Plan plan(const std::vector<Entry>& mods);

} // namespace gn::modorder
