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

### What instrumenting it showed

Printing from inside `appendRetroactiveConformances` before the fault:

```
[probe] conformances=2
[probe] ref opaque=0x61d5e60 invalid=0 abstract=1 concrete=0 pack=0
[probe] protocol=0x4814da0
<trap: memory access out of bounds inside isMarkerProtocol()>
```

So: the substitution map holds two conformances; the first is **abstract**, not concrete;
`getProtocol()` returns a pointer that is well-formed and 32-byte aligned, well inside
linear memory. The fault happens *after* that, inside

```cpp
bool ProtocolDecl::isMarkerProtocol() const {
  return getAttrs().hasAttribute<MarkerAttr>();
}
```

which walks the declaration's attribute list. That rules out the obvious first guess — a
mangled `ProtocolConformanceRef` — and points instead at either the `ProtocolDecl` pointer
being plausible-but-wrong (so `Attrs` is read from the wrong offset) or the attribute
chain itself being corrupt.

Measured on wasm32, `alignof(ProtocolConformance)` is 8 and
`PointerLikeTypeTraits<ProtocolConformance *>::NumLowBitsAvailable` is 3, so the
three-way `PointerUnion` in `ProtocolConformanceRef` has the bits it needs. **The
pointer-packing hypothesis below is therefore disproven for this type**, and the search
should move to how `AbstractConformance` is laid out and allocated on a 32-bit host.

What else is known:

* **Not stack depth.** Relinking with a 64 MiB stack and `--stack-first`, so an overflow
  would trap cleanly at address zero, reproduces the same fault at the same place.
* **Not the module cache or the sysroot.** The same compiler compiles other programs
  successfully against the same mounted sysroot.
* **32-bit-specific.** Two bugs of exactly this shape were already found and fixed here —
  `swift/lib/IRGen/Address.h` and `swift/lib/SILOptimizer/Utils/StackNesting.cpp` both
  packed a three-bit enum into an `llvm::Value *` / `SILInstruction *`, which a 64-bit host
  has room for and a 32-bit host does not. Those two announced themselves with static
  assertions. This one does not, so it is something that goes wrong silently: a layout or
  allocation assumption rather than a bit-packing one that the compiler can check.

The next step is to look at `AbstractConformance` — how it is laid out, and with what
alignment it is allocated in the ASTContext arena — since the failing conformance is
abstract and the protocol pointer it yields is the one that misbehaves.

## Consequences

Until this is fixed, the in-browser compiler handles simple programs and fails on most
real ones. The pipeline around it — argument vectors, sysroot, linker, execution — is
proven and does not change when the bug is fixed.

Separately, and independent of this bug: **macros and compiler plugins cannot work at
all**, because they are spawned executables (see [porting.md](porting.md)).
