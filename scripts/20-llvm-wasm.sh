#!/usr/bin/env bash
# Stage 20 — cross-build LLVM, clang and lld so that the compiler tools themselves run
# on a wasm host.
#
# `swift-frontend` embeds ClangImporter, so clang has to be cross-built for wasm before
# Stage 30 can link a frontend at all; and `wasm-ld` is what turns the frontend's object
# files into a program. Both come out of this one build tree.
#
# For a faster first signal, `LLVM_PROJECTS=lld` builds only the linker. That is the
# smallest useful piece of the compiler half and still exercises every hard part of the
# cross build — no threads, no fork/exec, no signals, WASI's filesystem, LLVM's platform
# layer on wasi-libc — in tens of minutes rather than hours. The full build reuses the
# same tree, so nothing is thrown away by starting narrow.
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

BUILD_DIR="${TC_BUILD}/wasm-llvm"
HOST_BIN="${TC_BUILD}/host/bin"

# Which LLVM subprojects to cross-build. "lld" alone is the fast validation path;
# "clang;lld" is what Stage 30 requires.
: "${LLVM_PROJECTS:=clang;lld}"

[[ -x "${HOST_BIN}/llvm-tblgen" && -x "${HOST_BIN}/clang-tblgen" ]] ||
  die "run 10-host-tools.sh first"
[[ -x "${WASI_SDK_PATH}/bin/clang" ]] || die "run 00-fetch-sources.sh first"

# wasi-libc provides mman/signal/process-clocks/getpid only as opt-in emulation
# libraries; LLVM's Support layer uses all four.
WASI_EMULATION_DEFINES="-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID"
WASI_EMULATION_LIBS="-lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks -lwasi-emulated-getpid"

# 16 MiB of stack: LLVM's recursive descent over IR and lld's graph walks overflow the
# 64 KiB wasm default long before they do anything interesting.
STACK_SIZE=16777216

# LLVM's platform detection only knows Win32/Unix/Generic, and wasi-sdk's CMake
# platform module sets neither, so configuration aborts with "Unable to determine
# platform". wasi-libc is POSIX-shaped, which is what LLVM_ON_UNIX actually means
# here, so declare it rather than patching HandleLLVMOptions.cmake.
log "configuring ${LLVM_PROJECTS} for ${WASM_TARGET}"
cmake -G Ninja -S "${LLVM_SRC}/llvm" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="${WASI_SDK_PATH}/share/cmake/wasi-sdk-p1.cmake" \
  -DWASI_SDK_PREFIX="${WASI_SDK_PATH}" \
  -DUNIX=1 \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="${LLVM_PROJECTS}" \
  -DLLVM_TARGETS_TO_BUILD=WebAssembly \
  -DLLVM_NATIVE_TOOL_DIR="${HOST_BIN}" \
  -DLLVM_DEFAULT_TARGET_TRIPLE="${WASM_TARGET}" \
  -DLLVM_HOST_TRIPLE="${WASM_TARGET}" \
  -DLLVM_ENABLE_THREADS=OFF \
  -DLLVM_ENABLE_PIC=OFF \
  -DLLVM_ENABLE_ZLIB=OFF \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF \
  -DLLVM_ENABLE_LIBPFM=OFF \
  -DLLVM_ENABLE_LIBEDIT=OFF \
  -DLLVM_ENABLE_CRASH_OVERRIDES=OFF \
  -DLLVM_ENABLE_BACKTRACES=OFF \
  -DLLVM_ENABLE_UNWIND_TABLES=OFF \
  -DLLVM_BUILD_TOOLS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_UTILS=OFF \
  -DLLVM_TOOL_LLVM_DRIVER_BUILD=OFF \
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
  -DCLANG_ENABLE_ARCMT=OFF \
  -DCMAKE_INSTALL_PREFIX="${TC_OUT}/wasm-llvm" \
  -DCMAKE_CXX_FLAGS="${WASI_EMULATION_DEFINES} -fno-exceptions" \
  -DCMAKE_C_FLAGS="${WASI_EMULATION_DEFINES}" \
  -DCMAKE_EXE_LINKER_FLAGS="${WASI_EMULATION_LIBS} -Wl,-z,stack-size=${STACK_SIZE}"

# What Stage 30 actually links: LLVM and clang *libraries*, plus lld. Deliberately NOT
# the clang driver binary — it drags in cc1_main's getrlimit and Apple's socket-based
# cc1depscan daemon, neither of which exists on WASI, for a binary swift-frontend never
# calls. The aggregate targets keep this robust across LLVM versions.
targets=(lld)
if [[ "$LLVM_PROJECTS" == *clang* ]]; then
  targets+=(llvm-libraries clang-libraries)
fi

log "building ${targets[*]} with ${JOBS} jobs (hours, with clang)"
ninja -C "$BUILD_DIR" -j "$JOBS" "${targets[@]}"

# lld is a multiplexed driver; invoked as wasm-ld it links wasm.
install -D "${BUILD_DIR}/bin/lld" "${TC_OUT}/wasm-ld.wasm"
log "wrote ${TC_OUT}/wasm-ld.wasm ($(du -h "${TC_OUT}/wasm-ld.wasm" | cut -f1))"

# Stage 30 configures against this build tree's CMake packages directly, so there is
# nothing to install.
