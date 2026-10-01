cmake_minimum_required(VERSION 3.18)
project(camoverride C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dobby (cloned into external/Dobby by the CI workflow), static build
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
add_subdirectory(external/Dobby)

add_library(camoverride SHARED src/main.cpp)
target_include_directories(camoverride PRIVATE src external/Dobby/include)
target_link_libraries(camoverride PRIVATE dobby log dl)
target_compile_options(camoverride PRIVATE -fvisibility=hidden -O2)
target_link_options(camoverride PRIVATE -Wl,--exclude-libs,ALL)
