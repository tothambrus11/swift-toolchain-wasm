#!/usr/bin/env bash
# Stage 30 — cross-build `swift-frontend` for a wasm host.
#
# Configure and compile are verified: Swift's own C++ sources build for
# wasm32-wasip1. The final link depends on Stage 20's LLVM and clang libraries.
#
# What had to be true, learned by doing rather than by reading:
#
#   * Swift's CMake already recognises WASI as a host — CMAKE_SYSTEM_NAME=WASI maps to
#     SWIFT_HOST_VARIANT_SDK=WASI. Nothing has to be taught that wasm-as-a-host exists.
#   * LLVM_TABLEGEN/CLANG_TABLEGEN must be passed explicitly, or Swift insists on an
#     ${LLVM_BINARY_DIR}/NATIVE directory and aborts with "no native LLVM build found".
#   * cmark-gfm must be *built*, not just checked out (Stage 15).
#   * SWIFT_WASI_SYSROOT_PATH must be wasi-sdk's sysroot, NOT the Swift SDK's WASI.sdk.
#     Their libc++ builds differ (_LIBCPP_HAS_THREADS 1 vs 0); LLVM is compiled against
#     wasi-sdk's, and llvm/Support/Mutex.h needs std::recursive_mutex.
#   * The wasi-libc emulation defines are required, not optional: swift's C++ includes
#     <signal.h>, which hard-errors without -D_WASI_EMULATED_SIGNAL.
#
# SWIFT_ENABLE_SWIFT_IN_SWIFT is OFF for this first working configuration. The compiler's
# Swift-implemented modules need C++ interop, and C++ interop is broken for
# wasm32-unknown-wasip1 in the stock Swift 6.3.3 SDK: importing any C++ module hits a
# Clang module cycle, "cyclic dependency in module 'SwiftWASILibc': SwiftWASILibc ->
# std_inttypes_h -> SwiftWASILibc". That reproduces with a two-line Swift file and the
# stock SDK, so it is an upstream SwiftWasm bug, not a misconfiguration here. Turning it
# off costs the Swift-implemented SIL optimizer passes — acceptable for a frontend that
# compiles at -Onone — and it is the switch to flip once interop is fixed.
#
# Macros need the compiler's Swift parser (ASTGen and the compiler's swift-syntax), and a
# native compiler runs macro plugins as processes or shared libraries, neither of which
# WASI has. With SWIFT_MACROS=1 this stage builds that parser in anyway, and patch 0003
# sends the in-process plugin channel to the embedder (tools/plugin-host), which runs
# plugins that are themselves wasm (Stage 37). The Swift code is compiled by the native
# swiftc for wasm32-wasip1 through CMake's Swift support, linked against the Swift SDK's
# runtime; SwiftCompilerSources stays off (SWIFT_ENABLE_SWIFT_IN_SWIFT=OFF), since
# macros do not need the Swift-written optimizer passes. Without SWIFT_MACROS=1 the
# build is exactly as before: SWIFT_BUILD_SWIFT_SYNTAX=OFF.
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

: "${SWIFT_SRC:=${TC_SRC}/swift}"
: "${SWIFT_CMARK_SRC:=${TC_SRC}/swift-cmark}"
: "${NATIVE_SWIFT_BIN:=${HOME}/.local/share/swiftly/toolchains/${SWIFT_VERSION}/usr/bin}"
: "${CMARK_INSTALL:=${TC_BUILD}/wasm-cmark-install}"
# Deliberately wasi-sdk's sysroot, NOT the Swift SDK's WASI.sdk, even though the latter
# is where the prebuilt stdlib lives. The two ship different libc++ configurations:
# wasi-sdk's wasm32-wasip1 libc++ sets _LIBCPP_HAS_THREADS=1, the Swift SDK's sets it to
# 0. LLVM and clang are cross-built against wasi-sdk's, and LLVM's Support/Mutex.h uses
# std::recursive_mutex unconditionally, so pointing Swift at the Swift SDK's sysroot
# fails with "no type named 'recursive_mutex' in namespace 'std'" — and linking two
# different libc++ ABIs into one binary would be worse than the error. One libc++ for
# all C++ in the toolchain; the Swift SDK supplies the Swift side only.
: "${SWIFT_WASM_SDK_SYSROOT:=${WASI_SDK_PATH}/share/wasi-sysroot}"

LLVM_BUILD="${TC_BUILD}/wasm-llvm"   # produced by 20-llvm-wasm.sh
SWIFT_BUILD="${TC_BUILD}/wasm-swift"
HOST_BIN="${TC_BUILD}/host/bin"

[[ -d "${SWIFT_SRC}" ]] || die "swift sources missing; extend 00-fetch-sources.sh"
[[ -x "${HOST_BIN}/llvm-tblgen" ]] || die "run 10-host-tools.sh first"
command -v "${NATIVE_SWIFT_BIN}/swiftc" >/dev/null 2>&1 ||
  warn "no native swiftc at ${NATIVE_SWIFT_BIN}; CROSSCOMPILE bootstrapping will fail"

WASI_EMULATION_DEFINES="-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID"
WASI_EMULATION_LIBS="-lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks -lwasi-emulated-getpid"
STACK_SIZE=16777216

# --- 1. LLVM + clang + lld for the wasm host ----------------------------------
# Built by Stage 20; this stage only checks it is there.
[[ -d "${LLVM_BUILD}/lib/cmake/llvm" ]] ||
  die "run 20-llvm-wasm.sh with LLVM_PROJECTS='clang;lld' first"

# --- 2. swift-frontend --------------------------------------------------------
[[ -f "${CMARK_INSTALL}/lib/cmake/cmark-gfm-config.cmake" ]] ||
  die "run 15-cmark-wasm.sh first"

MACRO_ARGS=(-DSWIFT_BUILD_SWIFT_SYNTAX=OFF)
if [[ "${SWIFT_MACROS:-0}" == "1" ]]; then
  : "${SWIFT_SYNTAX_SRC:=${TC_SRC}/swift-syntax}"
  : "${SWIFT_STRING_PROCESSING_SRC:=${TC_SRC}/swift-experimental-string-processing}"
  [[ -d "$SWIFT_SYNTAX_SRC" && -d "$SWIFT_STRING_PROCESSING_SRC" ]] ||
    die "macros need swift-syntax and swift-experimental-string-processing; run 00-fetch-sources.sh with SWIFT_MACROS=1"
  [[ -x "${NATIVE_SWIFT_BIN}/swiftc" ]] || die "macros need a native swiftc at ${NATIVE_SWIFT_BIN}"
  source "${TOOLCHAIN_ROOT}/scripts/wasm-sdk.sh"
  fetch_wasm_sdk
  SWIFT_HOST_SYSROOT="${TC_BUILD}/wasi-swift-host-sysroot"
  make_swift_host_sysroot "$SWIFT_HOST_SYSROOT"
  swift_flags="-sdk ${SWIFT_HOST_SYSROOT} -resource-dir ${WASM_SDK_BUNDLE}/swift.xctoolchain/usr/lib/swift_static"
  for define in $WASI_EMULATION_DEFINES -fno-exceptions; do swift_flags+=" -Xcc ${define}"; done
  log "macro support on: building ASTGen and the compiler's swift-syntax for ${WASM_TARGET}"
  MACRO_ARGS=(
    -DSWIFT_BUILD_SWIFT_SYNTAX=ON
    -DSWIFT_PATH_TO_SWIFT_SYNTAX_SOURCE="$SWIFT_SYNTAX_SRC"
    -DSWIFT_PATH_TO_STRING_PROCESSING_SOURCE="$SWIFT_STRING_PROCESSING_SRC"
    -DSWIFT_PATH_TO_SWIFT_SDK="${WASM_SDK_BUNDLE}/swift.xctoolchain/usr"
    -DCMAKE_Swift_COMPILER="${NATIVE_SWIFT_BIN}/swiftc"
    -DCMAKE_Swift_COMPILER_TARGET=wasm32-unknown-wasip1
    # CMake's check links a Swift executable with the toolchain file's C++ flags, which
    # are wasi-sdk's and not the Swift SDK's; the real links go through clang++.
    -DCMAKE_Swift_COMPILER_WORKS=ON
    -DCMAKE_Swift_FLAGS="$swift_flags"
  )
fi

log "configuring swift-frontend for ${WASM_TARGET}"
cmake -G Ninja -S "${SWIFT_SRC}" -B "$SWIFT_BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="${WASI_SDK_PATH}/share/cmake/wasi-sdk-p1.cmake" \
  -DWASI_SDK_PREFIX="${WASI_SDK_PATH}" \
  -DUNIX=1 \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR="${LLVM_BUILD}/lib/cmake/llvm" \
  -DClang_DIR="${LLVM_BUILD}/lib/cmake/clang" \
  -DLLVM_TABLEGEN="${HOST_BIN}/llvm-tblgen" \
  -DCLANG_TABLEGEN="${HOST_BIN}/clang-tblgen" \
  -Dcmark-gfm_DIR="${CMARK_INSTALL}/lib/cmake" \
  -Dcmark-gfm-extensions_DIR="${CMARK_INSTALL}/lib/cmake" \
  -DSWIFT_PATH_TO_CMARK_SOURCE="${SWIFT_CMARK_SRC}" \
  -DSWIFT_PATH_TO_CMARK_BUILD="${TC_BUILD}/wasm-cmark" \
  -DSWIFT_HOST_VARIANT_SDK=WASI \
  -DSWIFT_HOST_VARIANT_ARCH=wasm32 \
  -DSWIFT_WASI_SYSROOT_PATH="${SWIFT_WASM_SDK_SYSROOT}" \
  -DSWIFT_INCLUDE_TOOLS=ON \
  -DSWIFT_BUILD_STDLIB=OFF \
  -DSWIFT_BUILD_DYNAMIC_STDLIB=OFF \
  -DSWIFT_BUILD_STATIC_STDLIB=OFF \
  -DSWIFT_BUILD_REMOTE_MIRROR=OFF \
  -DSWIFT_BUILD_SOURCEKIT=OFF \
  -DSWIFT_INCLUDE_TESTS=OFF \
  -DSWIFT_INCLUDE_DOCS=OFF \
  "${MACRO_ARGS[@]}" \
  -DSWIFT_ENABLE_SWIFT_IN_SWIFT=OFF \
  -DBOOTSTRAPPING_MODE=OFF \
  -DSWIFT_USE_LINKER="" \
  -DSWIFT_NATIVE_CLANG_TOOLS_PATH="${NATIVE_SWIFT_BIN}" \
  -DCMAKE_CXX_FLAGS="${WASI_EMULATION_DEFINES} -fno-exceptions" \
  -DCMAKE_EXE_LINKER_FLAGS="${WASI_EMULATION_LIBS} -Wl,-z,stack-size=${STACK_SIZE} -Wl,--stack-first"

log "building swift-frontend"
ninja -C "$SWIFT_BUILD" -j "$JOBS" swift-frontend

install -D "${SWIFT_BUILD}/bin/swift-frontend" "${TC_OUT}/swift-frontend.wasm"
log "wrote ${TC_OUT}/swift-frontend.wasm ($(du -h "${TC_OUT}/swift-frontend.wasm" | cut -f1))"
