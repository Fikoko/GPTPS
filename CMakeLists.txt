cmake_minimum_required(VERSION 3.13)
project(gptps C)

# GPTPS - General Purpose Task Processing System.
# Embeddable, in-process C99 task-processing library. Core is strict C99;
# platform specifics live behind the HAL (see include/gptps_hal.h).

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# --- library (grows as modules land; currently HAL + config) ---------------
add_library(gptps STATIC
    src/hal_posix.c
    src/config.c
)
target_include_directories(gptps
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# POSIX feature macros for clock_gettime / sysconf / sysinfo under -std=c99.
if(UNIX)
    target_compile_definitions(gptps PRIVATE _GNU_SOURCE _POSIX_C_SOURCE=200809L)
endif()
if(NOT MSVC)
    target_compile_options(gptps PRIVATE -Wall -Wextra)
endif()

# --- tests (CTest) ---------------------------------------------------------
enable_testing()
add_executable(test_foundation tests/test_foundation.c)
target_include_directories(test_foundation
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_foundation PRIVATE gptps)
add_test(NAME foundation COMMAND test_foundation)
