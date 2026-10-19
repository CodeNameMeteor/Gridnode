// Compiles the generated gridnode/generated/types.hpp. Every struct in it carries static_asserts
// for its size and for the offsets of annotated members, so compiling it is the layout test.

#include <cstdio>

#include "gridnode/engine.hpp"
#include "gridnode/generated/types.hpp"

static_assert(sizeof(gn::fb::Array<int>) == 8, "Array");

int main() {
    std::puts("types_compile_test: types.hpp compiled with every layout check passing");
    return 0;
}
