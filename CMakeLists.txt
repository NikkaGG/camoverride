cmake_minimum_required(VERSION 3.18)
project(camoverride CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(camoverride SHARED src/main.cpp)
target_include_directories(camoverride PRIVATE src)
target_link_libraries(camoverride PRIVATE log dl)
target_compile_options(camoverride PRIVATE -fvisibility=hidden -O2)
target_link_options(camoverride PRIVATE -Wl,--exclude-libs,ALL)
