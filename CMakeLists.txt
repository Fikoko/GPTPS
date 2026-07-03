cmake_minimum_required(VERSION 3.13)
project(gptps VERSION 0.2.0 LANGUAGES C)

# GPTPS - General Purpose Task Processing System.
# Embeddable, in-process C99 task-processing library. Core is strict C99;
# platform specifics live behind the HAL (see include/gptps_hal.h).

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(WIN32)
    # mingw's <windows.h> relies on GNU extensions; the core itself stays C99-portable.
    set(CMAKE_C_EXTENSIONS ON)
endif()
if(MSVC)
    # MSVC has no /std:c99; C11 is its earliest selectable C mode and a superset of
    # the C99 the core targets. (Default MSVC is C89 and would reject C99 syntax.)
    set(CMAKE_C_STANDARD 11)
    # Quiet the C4996 deprecation noise MSVC emits for ISO C (fopen, ...) and POSIX
    # (_isatty, ...) names; the code uses them portably and correctly.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    # The test/example suite uses GCC __atomic_* builtins for cross-thread counters;
    # force-include a shim mapping them to Win32 Interlocked* under MSVC. Inert for
    # the library itself (it routes atomics through the HAL, not these builtins).
    add_compile_options(/FI${CMAKE_CURRENT_SOURCE_DIR}/tests/test_atomic_compat.h)
endif()

# --- library (grows as modules land) ---------------------------------------
# Core is portable C99; the HAL + executor backend are selected per platform.
set(GPTPS_SRC src/alloc.c src/config.c src/config_toml.c src/settings.c src/engine.c)
if(WIN32)
    list(APPEND GPTPS_SRC src/hal_win.c src/exec_win.c)        # Win32 HAL + CreateProcess/Job-Object program executor
else()
    list(APPEND GPTPS_SRC src/hal_posix.c src/exec_oop_posix.c) # POSIX HAL + out-of-process executor
endif()
add_library(gptps STATIC ${GPTPS_SRC})
target_include_directories(gptps
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# Feature-test macros are defined inside the HAL sources themselves (so the
# single-file amalgamation and a plain `cc -std=c99` build too), not here.
if(NOT MSVC)
    target_compile_options(gptps PRIVATE -Wall -Wextra)
endif()

# threads (HAL uses pthreads on POSIX)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(gptps PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# --- install / packaging (cmake --install; find_package(gptps); pkg-config) -
include(CMakePackageConfigHelpers)
install(TARGETS gptps EXPORT gptps-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/gptps.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT gptps-targets
        FILE gptps-targets.cmake NAMESPACE gptps::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gptps)
configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gptps-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/gptps-config.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gptps)
write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/gptps-config-version.cmake
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/gptps-config.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/gptps-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gptps)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/gptps.pc.in
               ${CMAKE_CURRENT_BINARY_DIR}/gptps.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/gptps.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# --- tests (CTest) ---------------------------------------------------------
enable_testing()
add_executable(test_constraint tests/test_constraint.c)
target_include_directories(test_constraint
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_constraint PRIVATE gptps)
add_test(NAME constraint COMMAND test_constraint)
set_tests_properties(constraint PROPERTIES TIMEOUT 30)

add_executable(test_cancel tests/test_cancel.c)
target_include_directories(test_cancel
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_cancel PRIVATE gptps)
add_test(NAME cancel COMMAND test_cancel)
set_tests_properties(cancel PROPERTIES TIMEOUT 30)

add_executable(test_service tests/test_service.c)
target_include_directories(test_service
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_service PRIVATE gptps)
add_test(NAME service COMMAND test_service)
set_tests_properties(service PROPERTIES TIMEOUT 30)

add_executable(test_backpressure tests/test_backpressure.c)
target_include_directories(test_backpressure
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_backpressure PRIVATE gptps)
add_test(NAME backpressure COMMAND test_backpressure)
set_tests_properties(backpressure PROPERTIES TIMEOUT 30)

add_executable(test_submit_ex tests/test_submit_ex.c)
target_include_directories(test_submit_ex
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_submit_ex PRIVATE gptps)
add_test(NAME submit_ex COMMAND test_submit_ex)
set_tests_properties(submit_ex PROPERTIES TIMEOUT 30)

add_executable(test_resource tests/test_resource.c)
target_include_directories(test_resource
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_resource PRIVATE gptps)
add_test(NAME resource COMMAND test_resource)
set_tests_properties(resource PROPERTIES TIMEOUT 30)

add_executable(test_foundation tests/test_foundation.c)
target_include_directories(test_foundation
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_foundation PRIVATE gptps)
add_test(NAME foundation COMMAND test_foundation)

add_executable(test_engine tests/test_engine.c)
target_include_directories(test_engine
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_engine PRIVATE gptps)
add_test(NAME engine COMMAND test_engine)
set_tests_properties(engine PROPERTIES TIMEOUT 30)

add_executable(test_failure tests/test_failure.c)
target_include_directories(test_failure
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_failure PRIVATE gptps)
add_test(NAME failure COMMAND test_failure)
set_tests_properties(failure PROPERTIES TIMEOUT 30)

add_executable(test_toml tests/test_toml.c) # config-file parsing + overrides
target_include_directories(test_toml
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_toml PRIVATE gptps)
target_compile_definitions(test_toml PRIVATE ADDON_DEMO_PATH="$<TARGET_FILE:addon_demo>")
add_test(NAME toml COMMAND test_toml)
set_tests_properties(toml PROPERTIES TIMEOUT 30)

add_executable(test_scheduler tests/test_scheduler.c) # priority + skip-to-fit + reservation
target_include_directories(test_scheduler
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_scheduler PRIVATE gptps)
add_test(NAME scheduler COMMAND test_scheduler)
set_tests_properties(scheduler PROPERTIES TIMEOUT 30)

add_executable(test_deadletter tests/test_deadletter.c) # dead-letter drain API
target_include_directories(test_deadletter
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_deadletter PRIVATE gptps)
add_test(NAME deadletter COMMAND test_deadletter)
set_tests_properties(deadletter PROPERTIES TIMEOUT 30)

add_executable(test_settings tests/test_settings.c) # unified settings registry
target_include_directories(test_settings
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_settings PRIVATE gptps)
add_test(NAME settings COMMAND test_settings)
set_tests_properties(settings PROPERTIES TIMEOUT 30)

add_executable(test_taskmgmt tests/test_taskmgmt.c) # runtime task management + generic settings
target_include_directories(test_taskmgmt
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_taskmgmt PRIVATE gptps)
add_test(NAME taskmgmt COMMAND test_taskmgmt)
set_tests_properties(taskmgmt PROPERTIES TIMEOUT 30)

add_executable(test_step tests/test_step.c) # MANUAL single-threaded pump (gptps_step)
target_include_directories(test_step
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_step PRIVATE gptps)
add_test(NAME step COMMAND test_step)
set_tests_properties(step PROPERTIES TIMEOUT 30)

add_executable(test_alloc tests/test_alloc.c) # process-wide allocator hook
target_include_directories(test_alloc
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_alloc PRIVATE gptps)
add_test(NAME alloc COMMAND test_alloc)
set_tests_properties(alloc PROPERTIES TIMEOUT 30)

# add-on shared libraries (compiled against the frozen header; no core link)
add_library(addon_demo MODULE tests/addon_demo.c)
target_include_directories(addon_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(addon_demo PROPERTIES PREFIX "")
add_library(addon_bad MODULE tests/addon_bad.c)
target_include_directories(addon_bad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(addon_bad PROPERTIES PREFIX "")

add_executable(test_addon tests/test_addon.c)
target_include_directories(test_addon
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_addon PRIVATE gptps)
target_compile_definitions(test_addon PRIVATE
    ADDON_DEMO_PATH="$<TARGET_FILE:addon_demo>"
    ADDON_BAD_PATH="$<TARGET_FILE:addon_bad>"
)
add_dependencies(test_addon addon_demo addon_bad)
add_test(NAME addon COMMAND test_addon)
set_tests_properties(addon PROPERTIES TIMEOUT 30)

add_dependencies(test_toml addon_demo) # test_toml writes the addon path into its config file

if(UNIX)
    add_executable(test_oop tests/test_oop.c)
    target_include_directories(test_oop
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(test_oop PRIVATE gptps)
    add_test(NAME oop COMMAND test_oop)
    set_tests_properties(oop PROPERTIES TIMEOUT 30)

    add_executable(test_result tests/test_result.c) # exercises the OOP executor too
    target_include_directories(test_result
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(test_result PRIVATE gptps)
    add_test(NAME result COMMAND test_result)
    set_tests_properties(result PROPERTIES TIMEOUT 30)

    add_executable(test_program tests/test_program.c) # external-program executor
    target_include_directories(test_program
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(test_program PRIVATE gptps)
    add_test(NAME program COMMAND test_program)
    set_tests_properties(program PROPERTIES TIMEOUT 30)

    add_executable(test_cgroup tests/test_cgroup.c) # cgroup v2 memory enforcement (self-skips w/o delegation)
    target_include_directories(test_cgroup
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(test_cgroup PRIVATE gptps)
    add_test(NAME cgroup COMMAND test_cgroup)
    set_tests_properties(cgroup PROPERTIES TIMEOUT 30)
endif()

# Portable add-on tests (all platforms): the add-on sources are portable
# (addon_compat shim); any fork-based PHASE inside a test is guarded for Windows.
add_executable(test_gpu_quota tests/test_gpu_quota.c addons/gpu_quota.c)  # constraint + observer
target_include_directories(test_gpu_quota PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/addons)
target_link_libraries(test_gpu_quota PRIVATE gptps)
add_test(NAME gpu_quota COMMAND test_gpu_quota)
set_tests_properties(gpu_quota PROPERTIES TIMEOUT 30)

add_executable(test_durable tests/test_durable.c addons/durable_queue.c) # journal happy-path everywhere; recovery forks on POSIX
target_include_directories(test_durable PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/addons)
target_link_libraries(test_durable PRIVATE gptps)
add_test(NAME durable COMMAND test_durable)
set_tests_properties(durable PROPERTIES TIMEOUT 30)

add_executable(test_orch tests/test_orch.c addons/gptps_orch.c)           # run-after / fan-in orchestration built on the public seams
target_include_directories(test_orch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/addons)
target_link_libraries(test_orch PRIVATE gptps)
add_test(NAME orch COMMAND test_orch)
set_tests_properties(orch PROPERTIES TIMEOUT 30)

add_executable(test_wasm tests/test_wasm.c addons/wasm_exec.c)            # module-as-task; OOP case POSIX-only
target_include_directories(test_wasm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/addons)
target_link_libraries(test_wasm PRIVATE gptps)
add_test(NAME wasm COMMAND test_wasm)
set_tests_properties(wasm PROPERTIES TIMEOUT 30)

add_executable(test_fuzz tests/test_fuzz.c addons/durable_queue.c)        # TOML + journal parser robustness
target_include_directories(test_fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/addons)
target_link_libraries(test_fuzz PRIVATE gptps)
add_test(NAME fuzz COMMAND test_fuzz)
set_tests_properties(fuzz PROPERTIES TIMEOUT 60)

# --- cross-platform external-program executor test (POSIX + Windows) -------
add_executable(prog_helper tests/prog_helper.c)         # tiny portable test binary
add_executable(test_program_helper tests/test_program_helper.c)
target_include_directories(test_program_helper PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_program_helper PRIVATE gptps)
target_compile_definitions(test_program_helper PRIVATE HELPER_PATH="$<TARGET_FILE:prog_helper>")
add_dependencies(test_program_helper prog_helper)
add_test(NAME program_helper COMMAND test_program_helper)
set_tests_properties(program_helper PROPERTIES TIMEOUT 30)

# real-time terminal dashboard add-on (portable; headless-tested)
add_executable(test_tui tests/test_tui.c addons/tui.c)
target_include_directories(test_tui
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/addons
)
target_link_libraries(test_tui PRIVATE gptps)
add_test(NAME tui COMMAND test_tui)
set_tests_properties(tui PROPERTIES TIMEOUT 30)

# --- examples (runnable usage; double as smoke tests) ----------------------
add_executable(demo examples/demo.c)
target_include_directories(demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(demo PRIVATE gptps)
add_test(NAME demo COMMAND demo)
set_tests_properties(demo PROPERTIES TIMEOUT 20)

add_executable(example_config examples/config_file.c)
target_include_directories(example_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(example_config PRIVATE gptps)
add_test(NAME example_config COMMAND example_config)
set_tests_properties(example_config PROPERTIES TIMEOUT 20)

# runtime control plane: enumerate / generic settings / clone / unregister
add_executable(example_task_control examples/task_control.c)
target_include_directories(example_task_control PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(example_task_control PRIVATE gptps)
add_test(NAME example_task_control COMMAND example_task_control)
set_tests_properties(example_task_control PROPERTIES TIMEOUT 20)

# single-thread + static memory pool (no worker threads, no libc heap)
add_executable(example_embedded examples/embedded.c)
target_include_directories(example_embedded PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(example_embedded PRIVATE gptps)
add_test(NAME example_embedded COMMAND example_embedded)
set_tests_properties(example_embedded PROPERTIES TIMEOUT 20)

# live terminal dashboard demo (self-skips the live loop without a TTY)
add_executable(example_dashboard examples/dashboard.c addons/tui.c)
target_include_directories(example_dashboard
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/addons)
target_link_libraries(example_dashboard PRIVATE gptps)
add_test(NAME example_dashboard COMMAND example_dashboard)
set_tests_properties(example_dashboard PROPERTIES TIMEOUT 20)

if(UNIX)
    # external-program executor (forks + execs /bin/cat)
    add_executable(example_program examples/external_program.c)
    target_include_directories(example_program PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(example_program PRIVATE gptps)
    add_test(NAME example_program COMMAND example_program)
    set_tests_properties(example_program PROPERTIES TIMEOUT 20)

    # run a .wasm module via a wasm runtime CLI through EXEC_PROGRAM
    # (self-skips, exit 0, when no `wasmtime` is on PATH)
    add_executable(example_wasm examples/wasm_program.c)
    target_include_directories(example_wasm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(example_wasm PRIVATE gptps)
    add_test(NAME example_wasm COMMAND example_wasm)
    set_tests_properties(example_wasm PROPERTIES TIMEOUT 20)
endif()
