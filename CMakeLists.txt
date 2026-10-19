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
)
target_include_directories(gn_reflect PUBLIC sdk/include)
set_target_properties(gn_reflect PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(GRIDNODE_BUILD_TESTS)
  enable_testing()
  add_executable(reflect_walker_test tests/reflect_walker_test.cpp)
  target_link_libraries(reflect_walker_test PRIVATE gn_reflect)
  add_test(NAME reflect_walker COMMAND reflect_walker_test)
endif()

# --- gn_offline_dump: run the walker on a PE memory dump of the game (developer tool, any OS) -------
add_executable(gn_offline_dump tools/offline_dump/offline_dump.cpp)
target_link_libraries(gn_offline_dump PRIVATE gn_reflect)

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
)
target_include_directories(gridnode PUBLIC sdk/include PRIVATE sdk/src)
target_link_libraries(gridnode PRIVATE gn_reflect)
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
