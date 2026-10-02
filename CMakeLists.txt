cmake_minimum_required (VERSION 3.5...3.31)
project (SHARPSAT)

option(STATIC "STATIC" OFF)
option(ALLOW_CLANG "Allow building with clang on macOS" OFF)
set(CMAKE_VERBOSE_MAKEFILE on)

# On macOS a bare `cmake ..` picks Apple clang (/usr/bin/g++ is a clang shim).
# This port is built and tested with Homebrew GCC, so refuse clang unless asked.
if(APPLE AND NOT ALLOW_CLANG AND (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_C_COMPILER_ID MATCHES "Clang"))
message(FATAL_ERROR "macOS builds use Homebrew GCC, but CMake picked clang "
  "(${CMAKE_CXX_COMPILER}). Run ./setupdev.sh, which selects the newest g++-N, "
  "or pass -DCMAKE_CXX_COMPILER=/opt/homebrew/bin/g++-N -DCMAKE_C_COMPILER=/opt/homebrew/bin/gcc-N "
  "to a fresh build directory. To build with clang anyway, pass -DALLOW_CLANG=ON.")
endif()

# macOS has no static libc, so -static cannot link there.
if(STATIC AND APPLE)
message(FATAL_ERROR "STATIC=ON is not supported on macOS; run ./setupdev.sh without 'static' (it selects Homebrew g++)")
endif()

if(STATIC)
set(CMAKE_EXE_LINKER_FLAGS -static)
set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
set(CMAKE_EXE_LINK_DYNAMIC_C_FLAGS)
set(CMAKE_EXE_LINK_DYNAMIC_CXX_FLAGS)
set(CMAKE_SHARED_LIBRARY_C_FLAGS)
set(CMAKE_SHARED_LIBRARY_CXX_FLAGS)
set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS)
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS)
set(BUILD_SHARED_LIBS OFF)
endif(STATIC)

set(CMAKE_CXX_FLAGS "-std=c++11 -O3 -Wall -Wextra -Wshadow -g -Wfatal-errors -Wno-sign-compare -Wno-literal-suffix")

SET(CMAKE_C_FLAGS "-std=c99 -Wall")

# clhash needs SSE4.1 + PCLMUL on x86; on ARM64 it goes through sse2neon.h,
# which uses PMULL when the crypto extension is enabled (every Apple Silicon
# CPU has it; other ARM64 CPUs may not, so there sse2neon's portable fallback
# is used unless the flag is added by hand). On macOS the target comes from
# CMAKE_OSX_ARCHITECTURES when set, since CMAKE_SYSTEM_PROCESSOR is the host.
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
list(LENGTH CMAKE_OSX_ARCHITECTURES SHARPSAT_NARCH)
if(SHARPSAT_NARCH GREATER 1)
message(FATAL_ERROR "Universal builds are not supported (clhash needs different flags per architecture); set one CMAKE_OSX_ARCHITECTURES")
endif()
set(SHARPSAT_ARCH ${CMAKE_OSX_ARCHITECTURES})
else()
set(SHARPSAT_ARCH ${CMAKE_SYSTEM_PROCESSOR})
endif()
if(SHARPSAT_ARCH MATCHES "^(x86_64|AMD64|amd64|i[3-6]86)$")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -msse4.1 -mpclmul")
elseif(APPLE AND SHARPSAT_ARCH MATCHES "^(arm64|aarch64)$")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv8-a+crypto")
endif()

# Only gmpxx.h is needed, not libgmpxx: every gmpxx call left is inline, and on
# macOS Homebrew's libgmpxx is built against libc++, which g++ does not use.
find_library(GMP_LIB gmp)
find_library(MPFR_LIB mpfr)
# Homebrew keeps headers outside the compiler's default search path.
find_path(GMP_INCLUDE_DIR gmpxx.h)
find_path(MPFR_INCLUDE_DIR mpfr.h)
if(NOT GMP_LIB OR NOT MPFR_LIB OR NOT GMP_INCLUDE_DIR OR NOT MPFR_INCLUDE_DIR)
message(FATAL_ERROR "GMP (including gmpxx.h) and MPFR are required")
endif()
include_directories(${GMP_INCLUDE_DIR} ${MPFR_INCLUDE_DIR})

add_definitions( -DMPFR_USE_NO_MACRO )
add_subdirectory(flowcutter)

SET(SOURCES 
	src/preprocessor/graph.cpp 
	src/preprocessor/instance.cpp
	src/preprocessor/oracle.cpp
	src/preprocessor/preprocessor.cpp
	src/preprocessor/subsumer.cpp
	src/preprocessor/treewidth.cpp
	src/preprocessor/twpp.cpp
	src/decomposition.cpp
	src/clhash/clhash.c
)

add_executable (sharpSAT src/main.cpp ${SOURCES})
if(STATIC)
SET_TARGET_PROPERTIES (sharpSAT PROPERTIES LINK_SEARCH_START_STATIC 1)
endif(STATIC)
target_link_libraries (sharpSAT ${MPFR_LIB} ${GMP_LIB})

add_executable (flow_cutter_pace17 flowcutter/pace.cpp)
if(STATIC)
SET_TARGET_PROPERTIES (flow_cutter_pace17 PROPERTIES LINK_SEARCH_START_STATIC 1)
endif(STATIC)
target_link_libraries (flow_cutter_pace17 libflowcutter)