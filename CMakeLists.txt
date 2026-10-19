cmake_minimum_required(VERSION 3.20)
project(gridnode VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "GridNode targets the 64-bit game: build for x64.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GRIDNODE_BUILD_TESTS "Build the host-side tests" ON)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # no VC++ redistributable needed
  add_compile_options(/W4 /permissive- /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra)
  if(WIN32)
    add_link_options(-static -static-libgcc -static-libstdc++)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
endif()

# --- gn_reflect: portable reflection walker (game-independent, testable on any OS) ---------------
add_library(gn_reflect STATIC
  sdk/src/reflect/walker.cpp
  sdk/src/reflect/dump_json.cpp
  sdk/src/reflect/registry.cpp
  sdk/src/hook_table.cpp
  sdk/src/dispatch.cpp
)
target_include_directories(gn_reflect PUBLIC sdk/include)
set_target_properties(gn_reflect PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(GRIDNODE_BUILD_TESTS)
  enable_testing()
  add_executable(reflect_walker_test tests/reflect_walker_test.cpp)
  target_link_libraries(reflect_walker_test PRIVATE gn_reflect)
  add_test(NAME reflect_walker COMMAND reflect_walker_test)
  add_executable(runtime_core_test tests/runtime_core_test.cpp)
  target_link_libraries(runtime_core_test PRIVATE gn_reflect)
  add_test(NAME runtime_core COMMAND runtime_core_test)
  find_package(Threads REQUIRED)
  target_link_libraries(runtime_core_test PRIVATE Threads::Threads)
endif()

# --- gn_offline_dump: run the walker on a PE memory dump of the game (developer tool, any OS) -------
add_executable(gn_offline_dump tools/offline_dump/offline_dump.cpp)
target_link_libraries(gn_offline_dump PRIVATE gn_reflect)

# --- generated headers (tools/gen, from db/annotations and your own dump) --------------------------
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(GN_GEN_DIR ${CMAKE_BINARY_DIR}/generated)
file(GLOB GN_ANNOTATION_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/db/annotations/*.yaml)
file(GLOB GN_GEN_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen/gridnode_gen/*.py)
set(GN_GEN ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen/gridnode.py)

# engine.hpp: annotations only (no dump needed). gridnode.dll's build check uses it.
add_custom_command(
  OUTPUT ${GN_GEN_DIR}/gridnode/engine.hpp
  COMMAND ${GN_GEN} engine --out ${GN_GEN_DIR}/gridnode/engine.hpp
  DEPENDS ${GN_ANNOTATION_FILES} ${GN_GEN_SOURCES}
  COMMENT "gridnode-gen: engine.hpp from db/annotations"
  VERBATIM)
add_custom_target(gn_engine_header DEPENDS ${GN_GEN_DIR}/gridnode/engine.hpp)
add_library(gn_engine INTERFACE)
target_include_directories(gn_engine INTERFACE ${GN_GEN_DIR})
add_dependencies(gn_engine gn_engine_header)

# types.hpp: needs your reflected dump (docs/dump-format.md). Built and compile-checked when present.
set(GRIDNODE_REFLECTED "${CMAKE_SOURCE_DIR}/db/reflected/gridnode-reflected.json" CACHE FILEPATH
    "Reflected dump to generate types.hpp from")
set(GRIDNODE_ANNOTATIONS "${CMAKE_SOURCE_DIR}/db/annotations" CACHE PATH "Annotation folder for types.hpp")
if(EXISTS "${GRIDNODE_REFLECTED}")
  file(GLOB GN_TYPES_ANNOTATION_FILES CONFIGURE_DEPENDS ${GRIDNODE_ANNOTATIONS}/*.yaml)
  add_custom_command(
    OUTPUT ${GN_GEN_DIR}/gridnode/generated/types.hpp
    COMMAND ${GN_GEN} --annotations ${GRIDNODE_ANNOTATIONS} types --reflected ${GRIDNODE_REFLECTED}
            --out ${GN_GEN_DIR}/gridnode/generated/types.hpp
    DEPENDS ${GRIDNODE_REFLECTED} ${GN_TYPES_ANNOTATION_FILES} ${GN_GEN_SOURCES}
    COMMENT "gridnode-gen: types.hpp from ${GRIDNODE_REFLECTED}"
    VERBATIM)
  add_custom_target(gn_types_header DEPENDS ${GN_GEN_DIR}/gridnode/generated/types.hpp)
  add_library(gn_types INTERFACE)
  target_include_directories(gn_types INTERFACE ${GN_GEN_DIR})
  add_dependencies(gn_types gn_types_header)
  if(GRIDNODE_BUILD_TESTS)
    add_executable(types_compile_test tests/types_compile_test.cpp)
    target_link_libraries(types_compile_test PRIVATE gn_types gn_engine)
    if(MSVC)
      target_compile_options(types_compile_test PRIVATE /bigobj)
    endif()
    add_test(NAME types_compile COMMAND types_compile_test)
  endif()
  message(STATUS "GridNode: types.hpp will be generated from ${GRIDNODE_REFLECTED}")
else()
  message(STATUS "GridNode: no reflected dump at ${GRIDNODE_REFLECTED}; types.hpp is not built (docs/dump-format.md)")
endif()

if(GRIDNODE_BUILD_TESTS)
  add_executable(engine_header_test tests/engine_header_test.cpp)
  target_link_libraries(engine_header_test PRIVATE gn_engine)
  add_test(NAME engine_header COMMAND engine_header_test)
  add_test(NAME gridnode_gen COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/tools/gen/tests -t ${CMAKE_SOURCE_DIR}/tools/gen)
endif()

if(NOT WIN32)
  return() # everything below is the Windows runtime
endif()

# --- gridnode.dll: the runtime loaded into the game ------------------------------------------------
add_library(gridnode SHARED
  sdk/src/dllmain.cpp
  sdk/src/runtime.cpp
  sdk/src/mem.cpp
  sdk/src/log.cpp
  sdk/src/build.cpp
  sdk/src/api.cpp
  sdk/src/ticks.cpp
  sdk/src/entities.cpp
  sdk/src/mods.cpp
)
target_include_directories(gridnode PUBLIC sdk/include PRIVATE sdk/src)
target_link_libraries(gridnode PRIVATE gn_reflect gn_engine)
set_target_properties(gridnode PROPERTIES OUTPUT_NAME "gridnode" PREFIX "")

# --- gridnode_inject.exe: loads gridnode.dll into the running game (development) -------------------
add_executable(gridnode_inject sdk/loader/inject.cpp)
target_compile_definitions(gridnode_inject PRIVATE UNICODE _UNICODE)
if(MSVC)
  target_link_options(gridnode_inject PRIVATE /ENTRY:wmainCRTStartup)
else()
  target_link_options(gridnode_inject PRIVATE -municode)
endif()

# --- version.dll: proxy loader (put in the game folder; loads gridnode.dll from there) -------------
add_library(version_proxy SHARED sdk/loader/version_proxy.cpp sdk/loader/version.def)
set_target_properties(version_proxy PROPERTIES OUTPUT_NAME "version" PREFIX "")

# --- example mods (output in mods/ next to gridnode.dll) ------------------------------------------
option(GRIDNODE_BUILD_EXAMPLES "Build the example mods" ON)
if(GRIDNODE_BUILD_EXAMPLES)
  add_library(hello SHARED examples/hello/hello.cpp)
  target_include_directories(hello PRIVATE sdk/include)
  target_link_libraries(hello PRIVATE gn_engine)
  set_target_properties(hello PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods
                                         RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods)
endif()
