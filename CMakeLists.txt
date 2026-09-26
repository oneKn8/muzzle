cmake_minimum_required(VERSION 3.16)
project(muzzle LANGUAGES CXX)

# The core has to build with the ESP32-C3's GCC 8.4, so it is held to C++17.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MUZZLE_SANITIZE "Build tests with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MUZZLE_LIBFUZZER "Build the libFuzzer target (clang only)" OFF)

set(MUZZLE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror)

# The core library: what runs on the microcontroller. No exceptions and no RTTI, so the
# host build proves it never depends on either.
add_library(muzzle_core src/feetech.cpp src/frame_parser.cpp)
target_include_directories(muzzle_core PUBLIC include)
target_compile_options(muzzle_core PRIVATE ${MUZZLE_WARNINGS} -fno-exceptions -fno-rtti)

if(MUZZLE_SANITIZE)
  set(MUZZLE_SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_compile_options(muzzle_core PRIVATE ${MUZZLE_SAN_FLAGS})
  target_link_options(muzzle_core PUBLIC -fsanitize=address,undefined)
endif()

enable_testing()

add_executable(muzzle_tests tests/test_main.cpp)
target_link_libraries(muzzle_tests PRIVATE muzzle_core)
target_compile_options(muzzle_tests PRIVATE ${MUZZLE_WARNINGS})
add_test(NAME unit COMMAND muzzle_tests)

# The differential fuzzer as a plain seeded random campaign, runnable with any compiler.
add_executable(muzzle_fuzz_random fuzz/fuzz_frame_parser.cpp)
target_link_libraries(muzzle_fuzz_random PRIVATE muzzle_core)
target_compile_options(muzzle_fuzz_random PRIVATE ${MUZZLE_WARNINGS})
add_test(NAME fuzz_random COMMAND muzzle_fuzz_random 20000)

if(MUZZLE_SANITIZE)
  target_compile_options(muzzle_tests PRIVATE ${MUZZLE_SAN_FLAGS})
  target_compile_options(muzzle_fuzz_random PRIVATE ${MUZZLE_SAN_FLAGS})
endif()

if(MUZZLE_LIBFUZZER)
  add_executable(muzzle_fuzz fuzz/fuzz_frame_parser.cpp src/feetech.cpp src/frame_parser.cpp)
  target_include_directories(muzzle_fuzz PRIVATE include)
  target_compile_definitions(muzzle_fuzz PRIVATE MUZZLE_LIBFUZZER)
  target_compile_options(muzzle_fuzz PRIVATE -g -O1 -fsanitize=fuzzer,address,undefined)
  target_link_options(muzzle_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

add_executable(muzzle_host_bench bench/host_bench.cpp)
target_link_libraries(muzzle_host_bench PRIVATE muzzle_core)
target_compile_options(muzzle_host_bench PRIVATE ${MUZZLE_WARNINGS})
