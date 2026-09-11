# Upstream status of every patch in this repository

Audited 2026-09-11 against `swiftlang/swift` `main` and `release/6.3`,
`llvm/llvm-project` `main`, `swiftlang/llvm-project` `next` and the
`swift-6.3.3-RELEASE` tag. Every "fixed"/"unfixed" claim below was checked by
reading the upstream file, not by reading a changelog.

The headline: **a wasm-hosted Swift compiler is no longer unexplored.** Between
this project's start and today, upstream landed an *Emscripten*-hosted toolchain
effort on `main`, and it fixes — independently, and in a nicer way — three of the
things patched here. None of it is in `release/6.3`, which is this repository's
base, so every patch is still required for the 6.3.3 build. What changes is the
upstreaming plan: most of it is already done by someone else, and what is left is
smaller and sharper than it looked.

## The two upstream efforts that matter

**[swiftlang/swift, July 2026 — Emscripten-hosted toolchain][forum-july]**
(@MaxDesiatov and the Swift-for-Wasm group). Merged to `main`:

| PR | What it did |
| --- | --- |
| [swift#90326] | Made the compiler's data structures 32-bit-safe: added `swift/Basic/PointerIntPair.h`, a `PointerIntPair` with a separate-storage fallback when the pointer has too few spare low bits, and force-aligned `AbstractConformance`. |
| [swift#90329] | Made immediate mode (`-interpret`) an optional build feature, `SWIFT_BUILD_IMMEDIATE_MODE`, for JIT-less hosts. |
| [swift#90332] | Fixed the Emscripten libc Clang module under C++ interop, and added UUID shims. |
| [swift#90334], [swift#90337] | CMake support and build-script products for an Emscripten host. |
| [swift#90760], [swift#90601], [swift#90801] | Path-leak and build-regression fixes for the Emscripten host. |

Their host is `wasm32-unknown-emscripten`, ours is `wasm32-unknown-wasip1`. That
difference is the whole story of what is left: Emscripten emulates the POSIX
process model (and ships `<uuid/uuid.h>`), WASI does not emulate any of it.

**[llvm/llvm-project#92677 — "Conditionalize use of POSIX features missing on
WASI/WebAssembly"][llvm92677]** (@whitequark, opened 2024-05-19, **still open**).
It is the direct ancestor of half the LLVM patch here, approved in principle by
aaronballman in 2024 and stalled since — first on wasi-sdk lacking `<mutex>`, then
on an unresolved argument about WASI-specific header directories versus inline
`#if`s, and now on rebase conflicts. In the meantime whitequark ships LLVM/clang/lld
for `wasm32-wasip1` out of tree as [YoWASP][yowasp] (currently LLVM 22.1.0), which
Compiler Explorer and others use. So the LLVM-on-wasm part of this project has real
prior art; only the Swift-on-WASI part did not.

## Swift patches (14 files)

### Fixed upstream on `main`, absent from `release/6.3`

| File | Upstream fix |
| --- | --- |
| `lib/IRGen/Address.h` | [swift#90326]. `StackAddress` now uses `swift::PointerIntPair<llvm::Value*, 3, Kind>` at the same line we patched. Our `#if __SIZEOF_POINTER__ < 8` hand-rolled storage is a worse version of the same idea — drop it on rebase. |
| `lib/SILOptimizer/Utils/StackNesting.cpp` | [swift#90326], same mechanism, same line. |
| `lib/AST/AbstractConformance.h` | [swift#90326]: `class alignas(1 << ConformanceAlignInBits) AbstractConformance`, with a new `ConformanceAlignInBits = 3` in `TypeAlignments.h`. Identical in effect to ours. |
| `SwiftCompilerSources/CMakeLists.txt` | `main` guards the `swift-stdlib-…` dependency with `if(TARGET …)`; the upstream comment reads "the swift-stdlib target is absent when building against a prebuilt target stdlib (e.g. a wasm host)". Same fix, independently. |

`release/6.3` has none of these: `include/swift/Basic/PointerIntPair.h` 404s on
that branch, `TypeAlignments.h` has no `ConformanceAlignInBits`, and
`AbstractConformance` is declared with no `alignas` at all. No backport PR to
`release/6.3` exists. There is no `release/6.4` branch yet.

### Fixed upstream for Emscripten only — WASI arm still missing

| File | Upstream state | What WASI still needs |
| --- | --- | --- |
| `lib/Basic/CMakeLists.txt` | `main` skips `find_package(UUID)` for `CMAKE_SYSTEM_NAME STREQUAL "Emscripten"` next to Darwin. | The same arm for `"WASI"`. One line. |
| `lib/Basic/UUID.cpp` | [swift#90332] shims `uuid_generate_random`/`uuid_generate_time` onto Emscripten's `uuid_generate`. | WASI has no `<uuid/uuid.h>` at all, so the shim has nothing to shim. Our `getentropy`-based implementation stands. |

### Not fixed upstream, and not reported

| File | Why it is WASI-only |
| --- | --- |
| `lib/Basic/Program.cpp` | `ExecuteInPlace` is gated on `LLVM_ON_UNIX`, and the spawn path on `HAVE_UNISTD_H`. WASI satisfies both and has neither `execve` nor `posix_spawn`. |
| `lib/Basic/TaskQueue.cpp` | Routes to `Unix/TaskQueue.inc`, which forks. WASI must fall through to the Default queue. |
| `lib/Basic/Statistic.cpp` | `HAVE_GETRUSAGE` is true on WASI; `getrusage` is not. |
| `lib/AST/PluginRegistry.cpp` | Signals a plugin process that cannot exist. |
| `lib/Driver/ToolChains.cpp` | Host detection is `#if defined(__APPLE__) \|\| defined(__unix__)` with an `#error Unknown compiler host` fallthrough. WASI defines neither. |
| `lib/IRGen/IRGen.cpp` | Spawns a `std::thread` purely to overlap releasing the SILModule with bitcode embedding. WASI has no threads; the work still has to happen, just inline. |

### A bug on every host, not just ours

`lib/Basic/Default/TaskQueue.inc` does not compile anywhere: it calls an
unqualified `make_unique` and an `llvm::sys::Wait(PI, unsigned, bool, ...)`
overload that no longer exists. Nothing builds the Default queue on a supported
platform, so the rot went unnoticed. Two-line fix, unreported upstream.

## Two latent bugs found while auditing, neither reported upstream

**1. `CompoundDeclName` and `SelectiveDeclNameRef` are under-aligned on any
32-bit host.** `include/swift/AST/Identifier.h` declares both with
`alignas(Identifier)` — the alignment of a `const char *`, four bytes on wasm32 —
while `PointerLikeTypeTraits<DeclName>` promises three spare low bits, which needs
eight. `Identifier` itself gets this right two hundred lines earlier: it defines
`RequiredAlignment = 1 << NumLowBitsAvailable` and `static_assert`s on it. The
symptom is garbled identifiers in diagnostics (`has no member 'D···r·'`), because
the tag bits land on top of pointer bits. Still present on `main` as of today.
Searching `swiftlang/swift` issues and PRs for this returns nothing, and
[swift#90326] did not touch it. Fix: `alignas(Identifier::RequiredAlignment)`.
This is the single cleanest thing this project has to contribute.

**2. `SILFunctionType` claims an alignment it does not have.**
`TypeAlignments.h` on `main`:

```cpp
LLVM_DECLARE_TYPE_ALIGNMENT(swift::SILFunctionType,
                            swift::TypeVariableAlignInBits)   // 4 bits = 16 bytes
```

`SILFunctionType` derives from `TypeBase`, which is aligned to
`TypeAlignInBits` = 3 bits = 8 bytes. The declaration therefore promises a spare
bit that does not exist — on 64-bit hosts as much as on wasm32; it just has not
bitten anyone yet. `scripts/55-check-alignment-contracts.sh` warns about this
rather than failing, precisely because it is an upstream bug and not ours. No
issue or PR reported.

## LLVM/clang patches (19 files)

### Fixed upstream

| File | Upstream fix |
| --- | --- |
| `llvm/include/llvm/Support/Compiler.h` | [llvm#215665] "[Support] Fix visibility macro guard: `__WASM__` is never defined" (merged 2026-08-27). The guard was spelled `__WASM__`, which no compiler defines, so wasm took the ELF visibility-attribute branch. Fixed to `__wasm__`. |
| `clang/include/clang/Support/Compiler.h` | Same PR, same typo. |

Both fixes are in `llvm/llvm-project` `main` **and** in `swiftlang/llvm-project`
`next`, but not at `swift-6.3.3-RELEASE`, which still reads `__WASM__`.

### Covered by the open PR llvm#92677

Ten of our nineteen files are the same problem that PR already solves:
`llvm/include/llvm/ADT/bit.h` (the `<endian.h>` include list, where sbc100's review
asked specifically for `__wasi__` rather than `__wasm__`),
`llvm/lib/Support/CrashRecoveryContext.cpp`, `LockFileManager.cpp`, `Signals.cpp`,
`Program.cpp`'s Unix backend, `Unix/Path.inc`, `Unix/Process.inc`, `Unix/Unix.h`,
`Unix/Watchdog.inc`, `raw_socket_stream.cpp`. It also touches `Unix/Memory.inc`,
`InitLLVM.cpp`, `config-ix.cmake`, `config.h.cmake`,
`ExecutionEngine/Interpreter/ExternalFunctions.cpp` and `clang/lib/Driver/Driver.cpp`,
which this build did not need. Unmerged for two years and three months.

The one real design difference is where the WASI code lives. llvm#92677
conditionalizes `Unix/Program.inc` and `Signals.cpp` in place with
`#if defined(__wasi__)`; this repository adds `llvm/lib/Support/WASI/Program.inc`
and `WASI/Signals.inc` and routes to them the way `Windows/` is routed to. That is
precisely the question the PR has been stuck on since 2024 — "WASI-specific header
directories versus inline `#if`s" — so this repository is, unintentionally, a
worked example of the other branch of that argument, and is worth posting as one.

### Not covered anywhere upstream

| File | Note |
| --- | --- |
| `llvm/lib/Support/ProgramStack.cpp` | `sigaltstack`/`getrlimit`. The file postdates llvm#92677. |
| `llvm/lib/CAS/OnDiskCommon.cpp` | `flock`/`F_OFD_SETLK`. |
| `clang/tools/CMakeLists.txt`, `llvm/tools/libCASPluginTest/CMakeLists.txt` | Skip tools that need a JIT or a shared library. |
| `clang/lib/Interpreter/RemoteJITUtils.cpp` | **The file no longer exists** on `llvm/llvm-project` `main` or `swiftlang/llvm-project` `next`. This hunk is dead on rebase. |

## The "no macros" limit has an upstream track too

`docs/status.md` lists macros and compiler plugins as impossible here, because plugins are
spawned executables and WASI cannot spawn anything. Two upstream changes bear on that:

* [swift#73725] "[Macros] In-process plugin server" (rintaro, merged 2024-06-26) removed
  the *spawn* requirement for the common case — the plugin server is loaded into the
  compiler's own process. It does not help a wasm host by itself, because loading it means
  `dlopen`ing `libSwiftInProcPluginServer.so`, and wasm modules are not dynamic libraries.
  (Our `lib/Driver/ToolChains.cpp` patch already teaches the path logic about this file on
  a WASI host; the loading is what is missing.)
* [swift#73031] "[Macros] Add support for wasm macros" (kabiroberai, later carried by
  MaxDesiatov) makes macro plugins themselves `.wasm` modules, executed by WasmKit inside
  `swift-plugin-server`. Approved by DougGregor and kateinoigakukun, still a draft as of
  June 2026. This is the shape that eventually works for us — with one twist: a
  wasm-*hosted* compiler cannot embed WasmKit sensibly (a wasm interpreter inside wasm),
  so the execution would want to be an import the embedder satisfies with the host's own
  `WebAssembly` API. That is a genuinely new piece of design, not a patch.

Neither is a blocker to fix; both are worth watching, and the second is where to push if
macros ever become a requirement for the IDE.

[swift#73725]: https://github.com/swiftlang/swift/pull/73725
[swift#73031]: https://github.com/swiftlang/swift/pull/73031

## What this means for the plan

1. **Rebasing onto `main` removes four Swift patches outright** (Address.h,
   StackNesting.cpp, AbstractConformance.h, SwiftCompilerSources/CMakeLists.txt)
   and one LLVM patch pair (`Compiler.h` ×2) — and replaces our hand-rolled
   32-bit storage with `swift::PointerIntPair`, which is better. Staying on 6.3.3
   keeps all of them.
2. **The C++-interop blocker has a known upstream fix we can copy.** We set
   `SWIFT_ENABLE_SWIFT_IN_SWIFT=OFF` because
   `SwiftWASILibc → std_inttypes_h → SwiftWASILibc` is a Clang module cycle.
   [swift#90332] solved exactly that for Emscripten by marking the module
   `[no_undeclared_includes]` and moving `complex.h` into a submodule.
   `stdlib/public/Platform/emscripten-libc.modulemap` on `main` reads
   `module SwiftEmscriptenLibc [system] [no_undeclared_includes]`;
   `wasi-libc.modulemap` on both `main` and `release/6.3` still reads
   `module SwiftWASILibc [system]` with `header "complex.h"` at the top level.
   Porting those two changes to `wasi-libc.modulemap` is a self-contained,
   obviously-correct upstream PR, and it is what would let the Swift-in-Swift
   parts of the compiler build for a WASI host.
3. **`SWIFT_BUILD_IMMEDIATE_MODE=OFF` ([swift#90329]) is the supported spelling**
   of something we currently get by accident from a narrow target list.
4. **Upstreaming order**, smallest and most defensible first:
   1. `Identifier.h` alignment — a real bug, trivially correct, host-independent.
   2. `Default/TaskQueue.inc` build fix — bit rot, host-independent.
   3. `wasi-libc.modulemap` — a direct port of an accepted Emscripten fix.
   4. `SILFunctionType` alignment — needs an upstream decision, not a patch from us.
   5. Swift's WASI `#if`s (Program, TaskQueue, Statistic, PluginRegistry,
      ToolChains, IRGen) — best offered as "WASI host, alongside the Emscripten
      host" once a WASI host SDK exists in-tree.
   6. LLVM's WASI support — the useful move is to help land [llvm#92677]
      (and to offer the `WASI/*.inc` layout as the resolution to its open design
      question), not to open a competing PR. `ProgramStack.cpp`,
      `CAS/OnDiskCommon.cpp` and the two CMake exclusions are small additions on
      top of it.

[forum-july]: https://forums.swift.org/t/swift-for-wasm-july-2026-updates/88673
[swift#90326]: https://github.com/swiftlang/swift/pull/90326
[swift#90329]: https://github.com/swiftlang/swift/pull/90329
[swift#90332]: https://github.com/swiftlang/swift/pull/90332
[swift#90334]: https://github.com/swiftlang/swift/pull/90334
[swift#90337]: https://github.com/swiftlang/swift/pull/90337
[swift#90601]: https://github.com/swiftlang/swift/pull/90601
[swift#90760]: https://github.com/swiftlang/swift/pull/90760
[swift#90801]: https://github.com/swiftlang/swift/pull/90801
[llvm92677]: https://github.com/llvm/llvm-project/pull/92677
[llvm#215665]: https://github.com/llvm/llvm-project/pull/215665
[yowasp]: https://www.npmjs.com/package/@yowasp/clang
