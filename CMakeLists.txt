cmake_minimum_required(VERSION 3.20)
project(gridnode VERSION 1.0.0 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "GridNode targets the 64-bit game: build for x64.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GRIDNODE_BUILD_TESTS "Build the host-side tests" ON)
# A player build: gridnode.dll as a mod loader only (the API, mods and their windows, saved layouts),
# without the inspector, console, Layout editor, free camera, Recorder or the type dump.
option(GRIDNODE_PLAYER_BUILD "Build gridnode.dll without the developer tools" OFF)

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
  sdk/src/config_core.cpp
  sdk/src/x64.cpp
  sdk/src/mod_order.cpp
  sdk/src/pe_file.cpp
  sdk/src/ant_types.cpp
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
  add_executable(x64_test tests/x64_test.cpp)
  target_link_libraries(x64_test PRIVATE gn_reflect)
  add_test(NAME x64 COMMAND x64_test)
  add_executable(vtable_count_test tests/vtable_count_test.cpp)
  add_test(NAME vtable_count COMMAND vtable_count_test)
  find_package(Threads REQUIRED)
  target_link_libraries(runtime_core_test PRIVATE Threads::Threads)
endif()

# --- gn_offline_dump: run the walker on a PE memory dump of the game (developer tool, any OS) -------
add_executable(gn_offline_dump tools/offline_dump/offline_dump.cpp)
target_link_libraries(gn_offline_dump PRIVATE gn_reflect)

# --- generated headers (tools/gen, from db/annotations and your own dump) --------------------------
find_package(Python3 COMPONENTS Interpreter REQUIRED)
# The generator needs PyYAML in the Python CMake found (which is not always the `python` on PATH).
execute_process(COMMAND ${Python3_EXECUTABLE} -c "import yaml" RESULT_VARIABLE GN_PYYAML_MISSING
                OUTPUT_QUIET ERROR_QUIET)
if(GN_PYYAML_MISSING)
  message(FATAL_ERROR "GridNode needs PyYAML in ${Python3_EXECUTABLE}:\n"
                      "  \"${Python3_EXECUTABLE}\" -m pip install pyyaml\n"
                      "or point CMake at a Python that has it: -DPython3_EXECUTABLE=<path to python.exe>")
endif()
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

# --- gn_imgui: Dear ImGui (third_party/imgui), for gridnode.dll and for mods that draw -------------
# Mods link this same copy: gridnode/ui.hpp checks that the version and layout match GridNode's.
add_library(gn_imgui STATIC
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
)
target_include_directories(gn_imgui PUBLIC third_party/imgui)
if(MSVC)
  target_compile_options(gn_imgui PRIVATE /W0)
else()
  target_compile_options(gn_imgui PRIVATE -w)
endif()

# --- gridnode.dll: the runtime loaded into the game ------------------------------------------------
add_library(gridnode SHARED
  sdk/src/dllmain.cpp
  sdk/src/runtime.cpp
  sdk/src/mem.cpp
  sdk/src/log.cpp
  sdk/src/build.cpp
  sdk/src/api.cpp
  sdk/src/ant.cpp
  sdk/src/replay.cpp
  sdk/src/sampler.cpp
  sdk/src/ticks.cpp
  sdk/src/entities.cpp
  sdk/src/mods.cpp
  sdk/src/unload.cpp
  sdk/src/patches.cpp
  sdk/src/quiet.cpp
  sdk/src/threads.cpp
  sdk/src/modsettings.cpp
  sdk/src/world.cpp
  sdk/src/bodies.cpp
  sdk/src/sounds.cpp
  sdk/src/tracer.cpp
  sdk/src/fielddiff.cpp
  sdk/src/gamestate.cpp
  sdk/src/ui.cpp
  sdk/src/inspector.cpp
  sdk/src/settings.cpp
  sdk/src/messages.cpp
  sdk/src/faith.cpp
  sdk/src/landing.cpp
  sdk/src/config.cpp
  sdk/src/hotkeys.cpp
  sdk/src/events.cpp
  sdk/src/assets.cpp
  sdk/src/layouts.cpp
  $<IF:$<BOOL:${GRIDNODE_PLAYER_BUILD}>,sdk/src/recorder_stub.cpp,sdk/src/recorder.cpp>
    sdk/src/crash.cpp
  third_party/imgui/backends/imgui_impl_dx11.cpp
  third_party/imgui/backends/imgui_impl_win32.cpp
)
set_source_files_properties(third_party/imgui/backends/imgui_impl_dx11.cpp third_party/imgui/backends/imgui_impl_win32.cpp
                            PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")
target_include_directories(gridnode PUBLIC sdk/include PRIVATE sdk/src third_party/imgui)
target_link_libraries(gridnode PRIVATE gn_reflect gn_engine gn_imgui d3d11 dxgi d3dcompiler gdi32 dwmapi)
# entities.cpp instantiates one detour per pool slot (2048 creators, 2048 destructors) and events.cpp
# 4096 more: MSVC's object file section limit is exceeded without /bigobj (CI, 2026-11-03).
if(MSVC)
  target_compile_options(gridnode PRIVATE /bigobj)
endif()
if(NOT MSVC)
  # cmpxchg16b for a detour's entry jump (patches.cpp); every CPU 64-bit Windows 8.1+ runs on has it.
  set_source_files_properties(sdk/src/patches.cpp PROPERTIES COMPILE_OPTIONS -mcx16)
endif()
set_target_properties(gridnode PROPERTIES OUTPUT_NAME "gridnode" PREFIX "")
target_compile_definitions(gridnode PRIVATE GRIDNODE_PLAYER_BUILD=$<BOOL:${GRIDNODE_PLAYER_BUILD}>)

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
  add_library(probe SHARED examples/probe/probe.cpp)
  target_include_directories(probe PRIVATE sdk/include)
  set_target_properties(probe PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods
                                         RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods)
  add_library(mec_trainer SHARED examples/mec-trainer/mec_trainer.cpp examples/mec-trainer/volumes.cpp)
  target_include_directories(mec_trainer PRIVATE sdk/include)
  target_link_libraries(mec_trainer PRIVATE gn_imgui gn_engine)
  set_target_properties(mec_trainer PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods
                                               RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods)
  add_library(world_draw SHARED examples/world-draw/world_draw.cpp)
  target_include_directories(world_draw PRIVATE sdk/include)
  target_link_libraries(world_draw PRIVATE gn_imgui)
  set_target_properties(world_draw PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods
                                              RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods)
  add_library(detour_demo SHARED examples/detour-demo/detour_demo.cpp)
  target_include_directories(detour_demo PRIVATE sdk/include)
  target_link_libraries(detour_demo PRIVATE gn_imgui gn_engine)
  set_target_properties(detour_demo PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods
                                               RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods)
  add_library(trainer SHARED examples/trainer/trainer.cpp)
  target_include_directories(trainer PRIVATE sdk/include)
  target_link_libraries(trainer PRIVATE gn_imgui)
  set_target_properties(trainer PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods
                                           RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods)
  # claim-lab: the one mod for trying research claims in game before they become SDK/API (one file per topic in experiments/)
  file(GLOB CLAIM_LAB_SOURCES CONFIGURE_DEPENDS examples/claim-lab/*.cpp examples/claim-lab/experiments/*.cpp)
  add_library(claim_lab SHARED ${CLAIM_LAB_SOURCES})
  target_include_directories(claim_lab PRIVATE sdk/include examples/claim-lab)
  target_link_libraries(claim_lab PRIVATE gn_imgui gn_engine)
  set_target_properties(claim_lab PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods
                                             RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods)
endif()
