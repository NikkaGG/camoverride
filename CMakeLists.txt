cmake_minimum_required(VERSION 3.18)
project(camoverride CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dobby (external/Dobby): build it as a STATIC library so it is linked into our .so.
# Different Dobby revisions name the targets differently, so pick whichever exists.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(external/Dobby EXCLUDE_FROM_ALL)

if (TARGET dobby_static)
  set(DOBBY_TARGET dobby_static)
elseif (TARGET dobby)
  set(DOBBY_TARGET dobby)
else ()
  message(FATAL_ERROR "No Dobby target found (expected dobby_static or dobby)")
endif ()
message(STATUS "Linking Dobby target: ${DOBBY_TARGET}")

add_library(camoverride SHARED src/main.cpp)
target_include_directories(camoverride PRIVATE src external/Dobby/include)
target_link_libraries(camoverride PRIVATE ${DOBBY_TARGET} log dl)
target_compile_options(camoverride PRIVATE -fvisibility=hidden -O2)
target_link_options(camoverride PRIVATE -Wl,--exclude-libs,ALL)
