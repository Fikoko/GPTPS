# A STANDALONE project - nothing in the GPTPS tree add_subdirectory's this. It is
# built by the `package` CI job against a staged INSTALL, which is the only way to
# prove the install tree is usable by someone who did not clone the repo.
#
#   cmake -S tests/consumer -B build -DCMAKE_PREFIX_PATH=/where/gptps/was/installed
cmake_minimum_required(VERSION 3.13)
project(consumer C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# COMPONENTS is the point: a consumer asks for the SUBSET it wants. Asking for one
# this install does not have must fail with a sentence that says so.
find_package(gptps 1.0 REQUIRED COMPONENTS durable_queue pool)

add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE gptps::durable_queue gptps::pool)
