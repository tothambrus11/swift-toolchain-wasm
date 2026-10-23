#!/usr/bin/env bash
# Stage 35 — cross-build sourcekitd, in process, for a wasm host.
#
# sourcekitd is the engine behind SourceKit-LSP: code completion, cursor info,
# diagnostics, all answered by the same compiler Stage 30 builds. This stage adds it to
# Stage 30's build tree rather than configuring a second one, so the Swift libraries it
# shares with swift-frontend (most of them) are compiled once.
#
# What SourceKit assumes that wasm32-wasip1 does not have, and what replaces it (the
# swift patch series, all of it WASI-only):
#
#   * libdispatch. SourceKit uses it through one small abstraction, WorkQueue and
#     Semaphore in SourceKit/Support/Concurrency.h. libdispatch has no WASI port at
#     6.3.3 and needs threads, so WASI gets Concurrency-cooperative.cpp instead: work is
#     queued and runs, in order, when somebody waits for it. A synchronous request is
#     exactly that shape, so sourcekitd_send_request_sync answers on one thread.
#   * Blocks. The C API's callbacks are blocks, so the Blocks runtime is still needed. It
#     is two portable C files in libdispatch's sources, built by SourceKit's CMake; that
#     is the only reason libdispatch is fetched at all.
#   * dladdr and dynamic linking. sourcekitdInProc is a static library on WASI, and the
#     toolchain prefix comes from $SOURCEKITD_TOOLCHAIN_PREFIX (default /usr) instead of
#     from where the library was loaded.
#   * XPC, sourcekitd-test. Neither exists here: XPC is Darwin's, and sourcekitd-test
#     spawns processes and parks in dispatch_main. sourcekitd-request is a minimal
#     driver that sends YAML requests synchronously and prints the responses.
#
# Why wasm32-wasip1 and not wasm32-wasip1-threads: everything this links against —
# LLVM, clang and the Swift libraries — is Stage 20/30's single-threaded wasip1 build.
# A threads build cannot link those objects (shared memory needs every object compiled
# with atomics), so it would mean rebuilding the whole toolchain a second time, and
# libdispatch would still need a WASI port first. Single-threaded also keeps the page
# free of the SharedArrayBuffer/COOP-COEP requirements, like the compiler itself.
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

: "${SWIFT_SRC:=${TC_SRC}/swift}"
: "${LIBDISPATCH_SRC:=${TC_SRC}/swift-corelibs-libdispatch}"

LLVM_BUILD="${TC_BUILD}/wasm-llvm"
SWIFT_BUILD="${TC_BUILD}/wasm-swift"

[[ -f "${SWIFT_BUILD}/CMakeCache.txt" ]] || die "run 30-swift-frontend-wasm.sh first"
[[ -f "${SWIFT_SRC}/tools/SourceKit/lib/Support/Concurrency-cooperative.cpp" ]] ||
  die "the swift patch series is not applied; run 05-apply-patches.sh"

if [[ -f "${LIBDISPATCH_SRC}/src/BlocksRuntime/runtime.c" ]]; then
  log "libdispatch sources already present"
else
  log "cloning swiftlang/swift-corelibs-libdispatch @ ${SWIFT_TAG} (for the Blocks runtime)"
  git clone --depth 1 --branch "$SWIFT_TAG" --single-branch \
    https://github.com/swiftlang/swift-corelibs-libdispatch.git "$LIBDISPATCH_SRC"
fi

# Re-run CMake on Stage 30's tree with SourceKit switched on. Everything else stays as
# Stage 30 cached it. Turning SourceKit on adds targets and changes no existing compile
# command (checked with `ninja -t commands swift-frontend` before and after: the only
# addition is SourceKit's generated UID header, an order-only dependency), so
# swift-frontend is not rebuilt. Stage 30 switches it off again on the next run; that
# costs a regeneration, not a rebuild.
log "configuring SourceKit in ${SWIFT_BUILD}"
cmake -S "$SWIFT_SRC" -B "$SWIFT_BUILD" \
  -DSWIFT_BUILD_SOURCEKIT=ON \
  -DSWIFT_PATH_TO_LIBDISPATCH_SOURCE="$LIBDISPATCH_SRC"

# The cooperative queues first: they are what makes sourcekitd work without threads at
# all, they take minutes rather than an hour to build, and the test says precisely what
# is wrong if they misbehave.
log "building SourceKitSupport"
ninja -C "$SWIFT_BUILD" -j "$JOBS" SourceKitSupport

runtime=""
for candidate in wasmtime wasmer; do
  command -v "$candidate" >/dev/null && { runtime="$candidate"; break; }
done

QUEUE_TEST="${TC_BUILD}/sourcekit-cooperative-queues.wasm"
log "building the cooperative work queue test"
"${WASI_SDK_PATH}/bin/clang++" --target="${WASM_TARGET}" -std=c++17 -O1 \
  -fno-exceptions -fno-rtti \
  -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL \
  -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID \
  -I "${SWIFT_SRC}/tools/SourceKit/include" \
  -I "${LLVM_SRC}/llvm/include" -I "${LLVM_BUILD}/include" \
  "${TOOLCHAIN_ROOT}/tests/sourcekit-cooperative-queues.cpp" \
  "${SWIFT_BUILD}/lib/libSourceKitSupport.a" \
  "${LLVM_BUILD}/lib/libLLVMSupport.a" "${LLVM_BUILD}/lib/libLLVMDemangle.a" \
  -lwasi-emulated-mman -lwasi-emulated-signal \
  -lwasi-emulated-process-clocks -lwasi-emulated-getpid \
  -o "$QUEUE_TEST"
if [[ -n "$runtime" ]]; then
  "$runtime" run "$QUEUE_TEST" || die "the cooperative work queues misbehave (see above)"
  # A wait nothing can satisfy must be reported, not hang a single-threaded host.
  if deadlock="$("$runtime" run "$QUEUE_TEST" deadlock 2>&1)"; then
    die "a wait that can never be satisfied returned instead of being reported"
  fi
  grep -q "would block forever" <<< "$deadlock" ||
    die "a wait that can never be satisfied failed without saying so: ${deadlock}"
  log "cooperative work queues behave"
else
  warn "no WASI runtime (install wasmtime); not running ${QUEUE_TEST}"
fi

log "building sourcekitd-request with ${JOBS} jobs"
ninja -C "$SWIFT_BUILD" -j "$JOBS" sourcekitdInProc sourcekitd-request

install -D "${SWIFT_BUILD}/bin/sourcekitd-request" "${TC_OUT}/sourcekitd-request.wasm"
log "wrote ${TC_OUT}/sourcekitd-request.wasm ($(du -h "${TC_OUT}/sourcekitd-request.wasm" | cut -f1))"
