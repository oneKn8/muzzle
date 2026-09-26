cmake_minimum_required(VERSION 3.16)
project(muzzle LANGUAGES CXX)

# The core has to build with the ESP32-C3's GCC 8.4, so it is held to C++17.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MUZZLE_SANITIZE "Build tests with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

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
