# swift-toolchain-wasm

The Swift compiler, cross-compiled to run **on** WebAssembly.

Not "Swift compiled *to* wasm" — that is [solved upstream][swift-wasm] and needs nothing
from this repository. This builds `swift-frontend` and `wasm-ld` as wasm modules, so a
browser tab can compile Swift with no server, the way
[abiexplorer.org](https://abiexplorer.org) does for clang.

## Artifacts

| File | What |
| --- | --- |
| `swift-frontend.wasm` | the Swift compiler frontend, running on wasm |
| `wasm-ld.wasm` | LLVM's wasm linker, running on wasm |
| `swift-sysroot-core.tar` | stdlib modules, archives, `swiftrt.o`, compiler-rt and wasi-libc |

Serve them next to a page and drive them over a virtual filesystem. There is no driver in
the pipeline: WASI cannot `fork`/`exec`, so the frontend and the linker are invoked as
separate modules with explicit argument vectors.

## Building

```sh
./scripts/build-all.sh          # everything, from nothing
BUILD_UNTIL=20-llvm-wasm ./scripts/build-all.sh   # stop early
```

Every stage is idempotent and caches into `build/`, so an interrupted build continues
instead of restarting. That is what makes the [CI workflow](.github/workflows/build-toolchain.yml)
possible at all: the LLVM and clang cross-build does not fit in one six-hour job, so the
workflow saves its build trees and resumes on the next run.

| Stage | Produces |
| --- | --- |
| `00-fetch-sources` | wasi-sdk, swiftlang/llvm-project, swiftlang/swift, swift-cmark |
| `05-apply-patches` | the `__wasi__` patch series (see below) |
| `10-host-tools` | native `llvm-tblgen`, `clang-tblgen` — any cross build needs these |
| `15-cmark-wasm` | cmark-gfm for the wasm host |
| `20-llvm-wasm` | LLVM + clang libraries + `wasm-ld.wasm` (hours) |
| `30-swift-frontend-wasm` | `swift-frontend.wasm` |
| `40-sysroot-pack` | the browser sysroot tarball |
| `50-smoke-test` | compiles, links and runs a Swift program using only the wasm tools |

Requirements: Linux x86_64, ~40 GB disk, cmake, ninja, a host C++ compiler, and a Swift
toolchain with the matching wasm SDK (Stage 40 takes the sysroot from the official SDK
bundle). See [docs/porting.md](docs/porting.md).

Every patch here has been audited against upstream `main` — four of them have since been
fixed upstream, two are latent bugs nobody has reported, and one has an accepted upstream
fix that only needs porting from Emscripten to WASI. See
[docs/upstream-status.md](docs/upstream-status.md).

## What had to be fixed

Building the Swift compiler for a wasm host surfaced three upstream bugs and one design
limit. All patches are guarded on `__wasi__`, so a native build of the same trees is
unaffected.

* **`LLVM_ABI` and `CLANG_ABI` are undefined on wasm.** Both `llvm/Support/Compiler.h` and
  `clang/Support/Compiler.h` end their export-macro chain with
  `defined(__MACH__) || defined(__WASM__) || defined(__EMSCRIPTEN__)`. No compiler defines
  `__WASM__` — clang spells it `__wasm__` — and wasm is not ELF, so no branch matches and
  the macros vanish. Consumers then fail to parse TableGen output with "variable has
  incomplete type 'class CLANG_ABI'". LLVM's own build escapes this by defining
  `LLVM_BUILD_STATIC`.
* **Swift requires libuuid on every non-Darwin, non-Windows host.** There is none for wasm,
  so `find_package(UUID REQUIRED)` finds the *build machine's* and puts `-I/usr/include` on
  every command line, where glibc's headers shadow the wasi sysroot's.
  `lib/Basic/UUID.cpp` now implements the operations directly for WASI.
* **C++ interop is broken for `wasm32-unknown-wasip1`** in the stock Swift 6.3.3 SDK: any
  C++ module import hits a Clang module cycle, `SwiftWASILibc -> std_inttypes_h ->
  SwiftWASILibc`. It reproduces with a two-line Swift file, so it is upstream. This is why
  `SWIFT_ENABLE_SWIFT_IN_SWIFT` is off: the compiler's Swift-implemented modules need
  interop. The cost is the Swift-implemented SIL optimizer passes.
* **Macros and compiler plugins cannot work.** Swift runs them by spawning a plugin
  executable and talking to it over a pipe, and WASI can spawn nothing — which is what
  `llvm/lib/Support/WASI/Program.inc` reports rather than pretending otherwise. Supporting
  them needs plugins redesigned as in-process wasm modules the embedder loads.

LLVM's platform layer also needed WASI implementations of the pieces that assume a POSIX
process model: `Program.inc` (no processes), `Signals.inc` (no signals), plus guards for
`setjmp`, `getrlimit`, `getsid`, `umask`, `fchown`, Unix sockets and terminal ioctls.

[swift-wasm]: https://www.swift.org/documentation/articles/wasm-getting-started.html
