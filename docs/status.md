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

## Known limits

* **Macros and compiler plugins cannot work.** They are spawned executables, and WASI
  cannot spawn anything — see [porting.md](porting.md). `SWIFT_BUILD_SWIFT_SYNTAX` is off.
* **No Swift-implemented SIL optimizer passes.** `SWIFT_ENABLE_SWIFT_IN_SWIFT` is off,
  because the compiler's own Swift modules need C++ interop and interop is broken for
  `wasm32-unknown-wasip1` in the stock Swift 6.3.3 SDK: any C++ module import hits a Clang
  module cycle, `SwiftWASILibc -> std_inttypes_h -> SwiftWASILibc`. That reproduces with a
  two-line Swift file against the stock SDK, so it is upstream.
* **4 GiB of address space.** Single-file compiles fit comfortably; whole-module builds of
  large packages may not.
* **Size.** ~300 MiB of artifacts on a cold load, before compression.
