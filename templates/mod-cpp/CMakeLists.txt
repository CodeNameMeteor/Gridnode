# @NAME@: a GridNode mod (made by `gridnode_mod.py new`).
#
#   cmake -S . -B build -A x64 -DGRIDNODE_SDK=<the SDK's sdk folder>
#   cmake --build build --config Release
#   python <sdk>/templates/gridnode_mod.py pack build/Release/@NAME@.dll   (or copy it into <game>\mods\@NAME@\)
#
# GRIDNODE_SDK is the `sdk` folder of a GridNode release (include\, imgui\, rust\).
cmake_minimum_required(VERSION 3.20)
project(@NAME@ LANGUAGES CXX)

set(GRIDNODE_SDK "@SDK@" CACHE PATH "The GridNode SDK folder (a release's sdk\\)")
if(NOT EXISTS "${GRIDNODE_SDK}/include/gridnode/api.h")
  message(FATAL_ERROR "GRIDNODE_SDK (${GRIDNODE_SDK}) is not a GridNode SDK folder: set -DGRIDNODE_SDK=<release>/sdk")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The game is 64-bit: build for x64.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dear ImGui, the same copy GridNode draws with (gridnode/ui.hpp checks the version): only for mods
# that draw. Set MOD_UI OFF for a mod without windows.
option(MOD_UI "The mod draws (gridnode/ui.hpp)" ON)

add_library(@NAME@ SHARED src/@NAME@.cpp)
target_include_directories(@NAME@ PRIVATE ${GRIDNODE_SDK}/include)
if(MOD_UI)
  add_library(@NAME@_imgui STATIC
    ${GRIDNODE_SDK}/imgui/imgui.cpp ${GRIDNODE_SDK}/imgui/imgui_draw.cpp
    ${GRIDNODE_SDK}/imgui/imgui_tables.cpp ${GRIDNODE_SDK}/imgui/imgui_widgets.cpp)
  target_include_directories(@NAME@_imgui PUBLIC ${GRIDNODE_SDK}/imgui)
  target_link_libraries(@NAME@ PRIVATE @NAME@_imgui)
endif()
set_target_properties(@NAME@ PROPERTIES PREFIX "")

if(MSVC)
  set_property(TARGET @NAME@ PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # no VC++ redistributable
  target_compile_options(@NAME@ PRIVATE /W4 /utf-8)
  target_compile_definitions(@NAME@ PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(@NAME@ PRIVATE -Wall -Wextra)
  target_link_options(@NAME@ PRIVATE -static -static-libgcc -static-libstdc++)
  target_compile_definitions(@NAME@ PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
