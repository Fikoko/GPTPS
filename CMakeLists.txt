cmake_minimum_required(VERSION 3.13)
project(gptps C)

# GPTPS - General Purpose Task Processing System.
# Embeddable, in-process C99 task-processing library. Core is strict C99;
# platform specifics live behind the HAL (see include/gptps_hal.h).

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# --- library (grows as modules land) ---------------------------------------
set(GPTPS_SRC src/hal_posix.c src/config.c src/config_toml.c src/engine.c)
if(UNIX)
    list(APPEND GPTPS_SRC src/exec_oop_posix.c) # out-of-process executor (POSIX)
endif()
add_library(gptps STATIC ${GPTPS_SRC})
target_include_directories(gptps
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
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

# --- tests (CTest) ---------------------------------------------------------
enable_testing()
add_executable(test_constraint tests/test_constraint.c)
target_include_directories(test_constraint
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_constraint PRIVATE gptps)
add_test(NAME constraint COMMAND test_constraint)
set_tests_properties(constraint PROPERTIES TIMEOUT 30)

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
endif()

# --- example (headline usage; doubles as a smoke test) ---------------------
add_executable(demo examples/demo.c)
target_include_directories(demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(demo PRIVATE gptps)
add_test(NAME demo COMMAND demo)
set_tests_properties(demo PROPERTIES TIMEOUT 20)
