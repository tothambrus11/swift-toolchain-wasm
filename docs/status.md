# Status: what the wasm-hosted Swift compiler does today

`swift-frontend.wasm` builds, runs, and compiles Swift to object code — for a subset of
the language. This records exactly where the line is, because "it works" and "it works
for everything" are very different claims.

## Works

`swift-frontend.wasm` (146 MiB) runs under a plain WASI runtime, reads its arguments,
loads the standard library from the mounted sysroot, type-checks, runs SIL, and emits a
wasm object file. A single-file compile takes about a second.

```
let x = 1                            ✅ compiles
func f(_ a: Int) -> Int { a * 2 }    ✅ compiles
struct S { var a: Int }              ✅ compiles
let s = "hi"                         ✅ compiles
```

`wasm-ld.wasm` (57 MiB) links object files into runnable programs, verified end to end:
a C object compiled by wasi-sdk, linked by wasm-ld running as wasm, and the resulting
program executed — no native tool in any step.

## Does not work yet

Anything whose mangling walks protocol conformances traps:

```
print("hello")                       ❌ RuntimeError: memory access out of bounds
let a = [1, 2, 3]                    ❌ RuntimeError: memory access out of bounds
```

The trap is inside name mangling:

```
RuntimeError: memory access out of bounds
  at swift::ProtocolDecl::isMarkerProtocol() const
  at swift::Mangle::ASTMangler::appendRetroactiveConformances(SubstitutionMap, GenericSignature)
  at swift::Mangle::ASTMangler::appendRetroactiveConformances(Type, GenericSignature)
  at swift::Mangle::ASTMangler::appendType(...)
```

What is known about it:

* **Not stack depth.** Relinking with a 64 MiB stack and `--stack-first`, so an overflow
  would trap cleanly at address zero, reproduces the same fault at the same place.
* **Not the module cache or the sysroot.** The same compiler compiles other programs
  successfully against the same mounted sysroot.
* **Almost certainly 32-bit pointer packing.** Two bugs of exactly this shape were already
  found and fixed by static assertions — `swift/lib/IRGen/Address.h` and
  `swift/lib/SILOptimizer/Utils/StackNesting.cpp` both packed a three-bit enum into an
  `llvm::Value *`/`SILInstruction *`, which a 64-bit host has room for and a 32-bit host
  does not. `ProtocolConformanceRef` holds a three-way `PointerUnion`, which needs two
  spare low bits — available only if every one of `AbstractConformance`,
  `ProtocolConformance` and `PackConformance` is allocated 4-byte aligned. A case where
  one is not would corrupt the pointer silently rather than firing an assertion, which
  matches the symptom exactly.

That last point is a hypothesis, not a diagnosis. Confirming it means checking the
alignment those conformance types are actually allocated with in the ASTContext arena on
a 32-bit host.

## Consequences

Until this is fixed, the in-browser compiler handles simple programs and fails on most
real ones. The pipeline around it — argument vectors, sysroot, linker, execution — is
proven and does not change when the bug is fixed.

Separately, and independent of this bug: **macros and compiler plugins cannot work at
all**, because they are spawned executables (see [porting.md](porting.md)).
