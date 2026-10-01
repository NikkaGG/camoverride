cmake_minimum_required(VERSION 3.18)
project(camoverride C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dobby (cloned into external/Dobby by the CI workflow).
# EXCLUDE_FROM_ALL: only the static target we link against gets built.
set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(external/Dobby EXCLUDE_FROM_ALL)

add_library(camoverride SHARED src/main.cpp)
target_include_directories(camoverride PRIVATE src external/Dobby/include)
if(TARGET dobby_static)
    target_link_libraries(camoverride PRIVATE dobby_static log dl)
else()
    target_link_libraries(camoverride PRIVATE dobby log dl)
endif()
target_compile_options(camoverride PRIVATE -fvisibility=hidden -O2)
target_link_options(camoverride PRIVATE -Wl,--exclude-libs,ALL)
