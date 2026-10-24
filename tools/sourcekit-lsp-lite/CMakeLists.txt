# sourcekit-lsp-lite: a thin LSP server over sourcekitd's C API.
#
# This builds it natively, against a Swift toolchain's libsourcekitdInProc.so,
# to develop and test it. The wasm build is scripts/36-sourcekit-lsp-lite-wasm.sh,
# which compiles the same sources with the WASI SDK and links them with Stage
# 35's static sourcekitd.
#
#   cmake --preset dev && cmake --build --preset dev && ctest --preset dev
#
# SOURCEKITD_PREFIX is the toolchain's "usr" directory (the one with
# lib/libsourcekitdInProc.so and include/SourceKit/sourcekitd.h). A toolchain
# without the header gets it from SOURCEKITD_INCLUDE_DIR (env or -D).

cmake_minimum_required(VERSION 3.25)
project(sourcekit-lsp-lite LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
include(StaticAnalyzers)

# Warnings and sanitizers, for everything here, tests included.
add_library(lite_options INTERFACE)
target_link_libraries(lite_options INTERFACE lite_warnings lite_sanitizers)

# Like the LLVM and Swift libraries the server is linked with on wasm: no
# exceptions, no RTTI. Nothing in it needs either. PRIVATE wherever it is
# used, so the test executables (native only) keep doctest's REQUIRE, which
# throws.
add_library(lite_no_exceptions INTERFACE)
target_compile_options(lite_no_exceptions INTERFACE -fno-exceptions -fno-rtti)

# --- sourcekitd ---------------------------------------------------------------
set(SOURCEKITD_PREFIX "$ENV{SOURCEKITD_PREFIX}" CACHE PATH
    "Swift toolchain usr directory with lib/libsourcekitdInProc.so")
find_path(SOURCEKITD_INCLUDE_DIR sourcekitd.h
  HINTS "$ENV{SOURCEKITD_INCLUDE_DIR}"
        "${SOURCEKITD_PREFIX}/include/SourceKit" "${SOURCEKITD_PREFIX}/include/sourcekitd"
  NO_DEFAULT_PATH REQUIRED)
find_library(SOURCEKITD_LIBRARY sourcekitdInProc
  HINTS "${SOURCEKITD_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
add_library(sourcekitd SHARED IMPORTED)
set_target_properties(sourcekitd PROPERTIES
  IMPORTED_LOCATION "${SOURCEKITD_LIBRARY}"
  # SYSTEM: the C API's own macros (C casts) are not ours to warn about.
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${SOURCEKITD_INCLUDE_DIR}"
  INTERFACE_INCLUDE_DIRECTORIES "${SOURCEKITD_INCLUDE_DIR}")
get_filename_component(SOURCEKITD_LIBRARY_DIR "${SOURCEKITD_LIBRARY}" DIRECTORY)

# --- the server -----------------------------------------------------------------
# Pure: JSON, positions, framing, options, request building and response
# translation. Tested without sourcekitd.
add_library(lite_core STATIC
  src/json.cpp
  src/text.cpp
  src/framing.cpp
  src/options.cpp
  src/requests.cpp
  src/translate.cpp)
target_include_directories(lite_core PUBLIC src)
target_link_libraries(lite_core PUBLIC lite_options PRIVATE lite_no_exceptions)
lite_analyze(lite_core)

# The part that talks to sourcekitd.
add_library(lite_server STATIC
  src/skd.cpp
  src/server.cpp
  src/pump.cpp)
target_link_libraries(lite_server PUBLIC lite_core sourcekitd PRIVATE lite_no_exceptions)
lite_analyze(lite_server)

add_executable(sourcekit-lsp-lite src/main_stdio.cpp)
target_link_libraries(sourcekit-lsp-lite PRIVATE lite_server lite_no_exceptions)
set_target_properties(sourcekit-lsp-lite PROPERTIES BUILD_RPATH "${SOURCEKITD_LIBRARY_DIR}")
lite_analyze(sourcekit-lsp-lite)

# The entry points a JavaScript host calls (the wasm reactor's exports),
# natively a library the reactor test links. src/frontend.cpp, the compiler's
# export in swift-frontend-lsp.wasm, is linked by Stage 36 only: it needs the
# compiler's own libraries, which no toolchain ships.
add_library(lite_reactor STATIC src/reactor.cpp)
target_link_libraries(lite_reactor PUBLIC lite_server PRIVATE lite_no_exceptions)
lite_analyze(lite_reactor)

# --- tests ----------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
