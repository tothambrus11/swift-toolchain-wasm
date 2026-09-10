#!/usr/bin/env bash
# Stage 10 — build the native LLVM host tools (TableGen) that the cross build needs.
#
# LLVM and clang generate a large amount of C++ from .td files using llvm-tblgen and
# clang-tblgen, which must run on the *build* machine. Cross-building therefore always
# starts with a native build of just those generators.
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

BUILD_DIR="${TC_BUILD}/host"

if [[ -x "${BUILD_DIR}/bin/llvm-tblgen" && -x "${BUILD_DIR}/bin/llvm-min-tblgen" \
      && -x "${BUILD_DIR}/bin/clang-tblgen" ]]; then
  log "native tblgen already built"
  exit 0
fi

log "configuring native host tools"
cmake -G Ninja -S "${LLVM_SRC}/llvm" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD=WebAssembly \
  -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF

log "building the TableGen executables with ${JOBS} jobs"
ninja -C "$BUILD_DIR" -j "$JOBS" llvm-tblgen llvm-min-tblgen clang-tblgen

log "host tools at ${BUILD_DIR}/bin"
