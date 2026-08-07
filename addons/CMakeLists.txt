# GPTPS add-ons - one library per module.
#
# WHY A LIBRARY AT ALL: until now every add-on was compiled as an extra SOURCE into
# a specific test binary - add_executable(test_pool tests/test_pool.c
# addons/gptps_pool.c) - so there was nothing for install() to ship and nothing a
# downstream project could link. The only documented way to obtain one was to clone
# the repo and vendor the raw .c at whatever commit you happened to have. An add-on
# you cannot obtain is not a module, it is a sample.
#
# WHY STATIC, unconditionally, and NOT BUILD_SHARED_LIBS: a compiled-in module calls
# core symbols DIRECTLY - that is what defines its tier. A shared add-on linked
# against a static core would carry its own copy of the core's file-statics, i.e.
# two engines in one process. The dlopen tier is the supported way to get a binary
# boundary, and it works precisely BECAUSE a plug-in links no core symbols at all.

set(GPTPS_ADDON_ALL await durable_queue gpu_quota orch pool remote tui wasm_exec xport)

# GPTPS_ADDONS is the human knob; the per-add-on GPTPS_ADDON_<NAME> booleans are what
# a distro packager scripts against. The list only sets their DEFAULTS.
#
# Guarded on NOT DEFINED, and that guard is load-bearing. A FetchContent consumer opts
# in with a plain `set(GPTPS_ADDONS "pool")` before add_subdirectory - but under
# CMP0126's default (OLD) behaviour, which this project inherits by supporting CMake
# 3.13, an unguarded `set(... CACHE ...)` here REMOVES that normal variable and the
# cache default wins. The consumer asked for one add-on and silently got all eight.
if(NOT DEFINED GPTPS_ADDONS)
    set(GPTPS_ADDONS "all" CACHE STRING
        "Add-ons to build: all | none | a ;-list from: ${GPTPS_ADDON_ALL}")
endif()

if(GPTPS_ADDONS STREQUAL "all")
    set(_want ${GPTPS_ADDON_ALL})
elseif(GPTPS_ADDONS STREQUAL "none")
    set(_want "")
else()
    set(_want ${GPTPS_ADDONS})
    # A typo must not silently give you nothing. This repo has twice rejected exactly
    # that failure mode in CI (the fail-closed TSan allowlist, the s390x include
    # list): a selector that quietly selects nothing reports success for work it
    # never did.
    foreach(a IN LISTS _want)
        list(FIND GPTPS_ADDON_ALL "${a}" _idx)
        if(_idx EQUAL -1)
            message(FATAL_ERROR "GPTPS_ADDONS: unknown add-on '${a}'. Known: ${GPTPS_ADDON_ALL}")
        endif()
    endforeach()
endif()

set(_built "")
function(gptps_addon name)
    cmake_parse_arguments(A "POSIX_ONLY" "SOURCE;HEADER;DESC" "" ${ARGN})
    string(TOUPPER "${name}" UC)
    list(FIND _want "${name}" _idx)
    if(_idx EQUAL -1)
        set(_def OFF)
    else()
        set(_def ON)
    endif()
    option(GPTPS_ADDON_${UC} "Build the ${name} add-on (${A_DESC})" ${_def})
    if(NOT GPTPS_ADDON_${UC})
        return()
    endif()

    if(A_POSIX_ONLY AND WIN32)
        # gptps_xport.c is fork() + socketpair(); on Windows the whole file is guarded
        # down to an EMPTY translation unit. Building it anyway would put a target in
        # the export set that links nothing, satisfy find_package(COMPONENTS xport)
        # with a lie, and trip MSVC's LNK4221 on the empty object. Not offering the
        # target is the honest answer; gptps-config.cmake turns a request for it into
        # a clear "this install has no xport".
        message(STATUS "gptps: skipping POSIX-only add-on '${name}' on this platform")
        return()
    endif()

    add_library(gptps_${name} STATIC ${A_SOURCE})
    add_library(gptps::${name} ALIAS gptps_${name})
    set_target_properties(gptps_${name} PROPERTIES
        EXPORT_NAME ${name}
        POSITION_INDEPENDENT_CODE ON)      # a host may link this into its OWN .so
    target_link_libraries(gptps_${name} PUBLIC gptps)
    target_include_directories(gptps_${name}
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/gptps>)
    if(NOT MSVC)
        target_compile_options(gptps_${name} PRIVATE -Wall -Wextra)
    endif()

    install(TARGETS gptps_${name} EXPORT gptps-targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
    # Under include/gptps/, not include/. `gptps_tui.h` is polite; dropping headers
    # straight into a shared /usr/local/include next to everyone else's is not.
    install(FILES ${A_HEADER} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gptps)

    set(GPTPS_ADDON_NAME ${name})
    set(GPTPS_ADDON_DESC "${A_DESC}")
    configure_file(${PROJECT_SOURCE_DIR}/cmake/gptps-addon.pc.in
                   ${PROJECT_BINARY_DIR}/gptps-${name}.pc @ONLY)
    install(FILES ${PROJECT_BINARY_DIR}/gptps-${name}.pc
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

    set(_built ${_built} ${name} PARENT_SCOPE)
endfunction()

gptps_addon(await         SOURCE gptps_await.c         HEADER gptps_await.h         DESC "blocking wait(handle) on the observer seam")
gptps_addon(durable_queue SOURCE gptps_durable_queue.c HEADER gptps_durable_queue.h DESC "crash-durable submission journal")
gptps_addon(gpu_quota     SOURCE gptps_gpu_quota.c     HEADER gptps_gpu_quota.h     DESC "named-resource admission quota")
gptps_addon(orch          SOURCE gptps_orch.c          HEADER gptps_orch.h          DESC "run-after / fan-in orchestration")
gptps_addon(pool          SOURCE gptps_pool.c          HEADER gptps_pool.h          DESC "N-shard engine router (scale-up)")
# remote is the wire CODEC only - pure byte transformations, no sockets and no
# platform calls, so it builds everywhere including Windows and the s390x leg that
# actually exercises its byte order.
gptps_addon(remote        SOURCE gptps_remote.c        HEADER gptps_remote.h        DESC "remote wire codec (cross-host transport)")
# tui is TTY-dependent at RUNTIME only - gptps_tui_run self-skips without a TTY, and
# render/press are headless and unit-tested. It compiles everywhere (POSIX termios and
# the Windows console are both handled inside the add-on), so there is nothing to gate.
gptps_addon(tui           SOURCE gptps_tui.c           HEADER gptps_tui.h           DESC "real-time terminal dashboard")
gptps_addon(wasm_exec     SOURCE gptps_wasm_exec.c     HEADER gptps_wasm_exec.h     DESC "module-as-task, pluggable runtime")
gptps_addon(xport         SOURCE gptps_xport.c         HEADER gptps_xport.h         DESC "worker-process transport (scale-out; POSIX)" POSIX_ONLY)

set(GPTPS_BUILT_ADDONS "${_built}" PARENT_SCOPE)
