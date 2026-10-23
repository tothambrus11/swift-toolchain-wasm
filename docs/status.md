# Status: what the wasm-hosted Swift compiler does today

`swift-frontend.wasm` and `wasm-ld.wasm` compile, link and run Swift in a browser tab.

## Works

```swift
let squares = (1...5).map { $0 * $0 }
print("squares: \(squares)")          // squares: [1, 4, 9, 16, 25]
```

That program is the end-to-end test: compiled by `swift-frontend.wasm`, linked by
`wasm-ld.wasm`, executed under a WASI shim, with its **stdout asserted**. No native tool
takes part in any step.

Also verified compiling: structs and protocol conformances (`CustomStringConvertible`),
dictionaries and sorting, string interpolation, closures, generics through `map`.

A single-file compile takes roughly one second in Node, about three in a browser tab.
`swift-frontend.wasm` is 146 MiB and `wasm-ld.wasm` is 57 MiB.

## The 32-bit bugs that had to be fixed first

Swift declares how many spare low bits a pointer to each AST type has:

```cpp
LLVM_DECLARE_TYPE_ALIGNMENT(swift::TypeBase, swift::TypeAlignInBits)
```

Nothing checks that the type is really that aligned. On a 64-bit host the promise is
often kept by accident — a couple of pointers give natural alignment 8, which is the
three bits usually claimed — so a type can be under-aligned on a 32-bit host and nobody
notices. Nothing asserts; pointers just come back subtly wrong.

Two types were breaking Swift on wasm32:

* **`AbstractConformance`** promised three bits but was allocated with
  `alignof(AbstractConformance)`: a `FoldingSetNode` plus two pointers, so 8 on a 64-bit
  host and **4** on wasm32. Symptom: `memory access out of bounds` inside
  `ProtocolDecl::isMarkerProtocol()`, reached from
  `ASTMangler::appendRetroactiveConformances`. This is what made `print()` and array
  literals uncompilable.
* **`CompoundDeclName`** (and `SelectiveDeclNameRef`) used `alignas(Identifier)` — the
  alignment of a `const char *`, which is 8 only on a 64-bit host. `DeclName` promises
  `DeclBaseName`'s three bits minus one for the union tag, so it needs eight-byte
  alignment; on wasm32 it got four. Symptom: the clobbered bit turned one name into
  another, and diagnostics printed identifiers as garbage —
  `value of type 'DefaultStringInterpolation' has no member 'D<?><?>r<?>'`.

Both now state the alignment their traits already claim, the way `ProtocolConformance`
and `Decl` do.

`scripts/55-check-alignment-contracts.sh` generates a `static_assert` per contract and
compiles it for the target. It is how both bugs were found, and it runs as part of
`build-all.sh` so a broken contract fails the build rather than producing a compiler that
miscompiles somewhere far away.

That script also reports that **`swift::SILFunctionType` violates its contract on every
host, 64-bit included** — a pre-existing upstream inconsistency that nothing appears to
exercise. It is warned about, not treated as a wasm regression.

## sourcekitd

**Status: builds in CI on a branch; not yet seen answering.** Stage 35 cross-builds the
in-process sourcekitd into Stage 30's tree and links `sourcekitd-request.wasm`; Stage 52
asks it for a completion and a cursor info under wasmtime. Until a run has passed Stage
52, everything below is the design, verified as far as can be without linking: CMake
configures, every SourceKit, IDETool and Refactoring object compiles for
`wasm32-wasip1`, every library the link needs is one Stage 20 builds, and the work queues
pass their unit test under a WASI runtime.

`sourcekitd-request.wasm` sends sourcekitd's YAML requests synchronously and prints the
responses. The compiler arguments are driver arguments, as an editor sends them:

```
{
  key.request: source.request.codecomplete,
  key.sourcefile: "/work/main.swift",
  key.offset: 42,
  key.compilerargs: ["/work/main.swift", "-target", "wasm32-unknown-wasip1",
                     "-sdk", "/sysroot/wasi-sysroot",
                     "-resource-dir", "/sysroot/swift/lib/swift_static",
                     "-module-cache-path", "/work/mcache"]
}
```

```
wasmtime run --dir work::/work --dir sysroot::/sysroot sourcekitd-request.wasm /work/complete.yaml
```

What SourceKit assumes that `wasm32-wasip1` lacks, and what the patch series does instead
(all of it WASI-only, so a native build of the same tree is unaffected):

| Assumes | Instead |
| --- | --- |
| libdispatch, on threads | `Concurrency-cooperative.cpp`: `WorkQueue`/`Semaphore` queue work and run it, in submission order, when somebody waits. Serial queues, barriers and suspension keep their meaning; a wait nothing can satisfy is reported, not hung. No libdispatch is built. |
| The Blocks runtime (the C API's callbacks are blocks) | Built alone from libdispatch's `src/BlocksRuntime`: two portable C files. |
| `dladdr`, to find the toolchain next to the library | `$SOURCEKITD_TOOLCHAIN_PREFIX`, default `/usr`. Requests normally pass `-resource-dir` anyway. |
| A shared `sourcekitdInProc` | A static one. |
| `dispatch_after`, for `SOURCEKIT_DELAY_SEMA_EDITOR` | Ignored, with a log line. |
| `sourcekitd-test` (spawns, threads, `dispatch_main`) | `sourcekitd-request`, about a hundred lines. |

Why `wasm32-wasip1` and not `wasm32-wasip1-threads`: sourcekitd links against the same
LLVM, clang and Swift libraries as `swift-frontend.wasm`, which are single-threaded
`wasip1` objects. A shared-memory module cannot link them — every object would need
atomics — so the threads target means building the whole toolchain a second time, and
libdispatch would still need a WASI port before it could use the threads. It would also
make the page require cross-origin isolation, which the compiler does not.

What single-threaded costs, and what an embedder has to know:

* **A request blocks its caller until it is answered.** Run sourcekitd in a worker.
* **Cancellation cannot interrupt a running request:** nothing else runs while it does.
* **Notifications** (document updates, semantic-info-ready) are queued work like any
  other. They run when the next request waits, or when the embedder calls
  `SourceKit::WorkQueue::runPendingWork()`. There is no C entry point for that yet; an
  embedding through the C API needs one.
* **A queued item that takes a `std::mutex` its waiter holds traps** (wasi-libc's
  single-threaded mutex has nothing to wait for). Nothing in the request paths used by
  completion and cursor info is known to do this, but it is the failure to expect.

## Known limits

* **Macros and compiler plugins cannot work.** They are spawned executables, and WASI
  cannot spawn anything — see [porting.md](porting.md). `SWIFT_BUILD_SWIFT_SYNTAX` is off.
* **No Swift-implemented SIL optimizer passes.** `SWIFT_ENABLE_SWIFT_IN_SWIFT` is off,
  because the compiler's own Swift modules need C++ interop and interop is broken for
  `wasm32-unknown-wasip1` in the stock Swift 6.3.3 SDK: any C++ module import hits a Clang
  module cycle, `SwiftWASILibc -> std_inttypes_h -> SwiftWASILibc`. That reproduces with a
  two-line Swift file against the stock SDK, so it is upstream. Upstream has since fixed
  the identical cycle for the Emscripten libc module — `[no_undeclared_includes]` plus
  moving `complex.h` into a submodule, [swift#90332] — and has not applied it to
  `wasi-libc.modulemap`, which still reads `module SwiftWASILibc [system]` on both `main`
  and `release/6.3`. Porting those two lines is the way out of this limit; see
  [upstream-status.md](upstream-status.md).

[swift#90332]: https://github.com/swiftlang/swift/pull/90332
* **4 GiB of address space.** Single-file compiles fit comfortably; whole-module builds of
  large packages may not.
* **Size.** ~300 MiB of artifacts on a cold load, before compression.
