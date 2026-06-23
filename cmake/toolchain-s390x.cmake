# toolchain-s390x.cmake - cross-build for IBM z/Architecture (s390x), a 64-bit
# BIG-ENDIAN target. Used by CI to exercise byte-order correctness (the journal /
# TOML / settings serialization and every fixed-width type) that the little-endian
# x86_64 + arm64 matrix never hits. Tests run under qemu-s390x-static.
#
#   cmake -S . -B build-s390x -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-s390x.cmake
#   cmake --build build-s390x
#   ctest --test-dir build-s390x   # binaries auto-run under the emulator below
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR s390x)

set(CMAKE_C_COMPILER   s390x-linux-gnu-gcc)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-s390x-static)

set(CMAKE_FIND_ROOT_PATH /usr/s390x-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
