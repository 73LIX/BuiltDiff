cmake_minimum_required(VERSION 3.16)
project(builtdiff VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(BUILTDIFF_SANITIZE "Build with ASan + UBSan" OFF)
option(BUILTDIFF_BUILD_TESTS "Build the test suite" ON)

find_package(Threads REQUIRED)

add_library(builtdiff_core STATIC
  src/util.cpp src/json.cpp src/proc.cpp src/elf.cpp src/knowledge.cpp src/sysinfo.cpp
  src/scan.cpp src/snapshot.cpp src/compare.cpp src/http.cpp src/analyze.cpp src/render.cpp
  src/ecosystems.cpp)
target_include_directories(builtdiff_core PUBLIC src)
target_link_libraries(builtdiff_core PUBLIC Threads::Threads)
target_compile_options(builtdiff_core PUBLIC -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat=2
  -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE)
target_link_options(builtdiff_core PUBLIC -pie -Wl,-z,relro,-z,now)
if(BUILTDIFF_SANITIZE)
  target_compile_options(builtdiff_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g)
  target_link_options(builtdiff_core PUBLIC -fsanitize=address,undefined)
endif()

add_executable(builtdiff src/main.cpp)
target_link_libraries(builtdiff PRIVATE builtdiff_core)
install(TARGETS builtdiff RUNTIME DESTINATION bin)

if(BUILTDIFF_BUILD_TESTS)
  enable_testing()
  add_executable(builtdiff_tests tests/tests.cpp)
  target_link_libraries(builtdiff_tests PRIVATE builtdiff_core)
  add_test(NAME unit COMMAND builtdiff_tests)
endif()
