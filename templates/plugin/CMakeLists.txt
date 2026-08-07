# A STANDALONE project. Nothing in the GPTPS tree add_subdirectory's this - copy the
# directory somewhere else and build it against an INSTALLED GPTPS:
#
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=/where/you/installed/gptps
#   cmake --build build
#   gptps_conformance build/myplugin.so      # prove it before you ship it
cmake_minimum_required(VERSION 3.13)
project(myplugin C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(gptps 1.0 REQUIRED)

add_library(myplugin MODULE myplugin.c)

# myplugin.so, not libmyplugin.so: a config file names a PATH, not a -l flag.
set_target_properties(myplugin PROPERTIES PREFIX "")

# Take the INCLUDE path from the imported target - and deliberately do NOT link it.
#
# A plug-in links no core symbols; that is the entire point of the host table, and
# what lets one .so work against a static, shared or amalgamated host. Linking the
# core here would put a SECOND copy of it inside the plug-in: two sets of file
# statics, two engines, one process.
#
# ($<COMPILE_ONLY:gptps::gptps> says this in one word but needs CMake 3.27; this
# stays at 3.13 to match GPTPS itself.)
target_include_directories(myplugin PRIVATE
    $<TARGET_PROPERTY:gptps::gptps,INTERFACE_INCLUDE_DIRECTORIES>)

# Export gptps_addon_init and nothing else. GPTPS_ADDON_EXPORT already carries the
# visibility attribute; this makes hidden the default for everything else, so a
# plug-in cannot accidentally capture (or be captured by) symbols from the host or
# from another plug-in loaded after it.
if(NOT MSVC)
    target_compile_options(myplugin PRIVATE -fvisibility=hidden -Wall -Wextra)
endif()
