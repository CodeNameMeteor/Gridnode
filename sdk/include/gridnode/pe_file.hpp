#pragma once
// Reading a DLL file's export table from its bytes, without loading it (mods.cpp: whether a mod
// exports gn_mod_unload). Portable: tested on the host (tests/runtime_core_test.cpp).

#include <cstddef>
#include <cstdint>

namespace gn::pe {

// Whether the PE32+ image in `file` (the file's bytes, as on disk) exports `name`. False for
// anything malformed: every offset is checked against the size.
bool exportsName(const uint8_t* file, size_t size, const char* name);

} // namespace gn::pe
