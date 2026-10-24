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

## Macros (`SWIFT_MACROS=1`, workflow input `macros: true`)

A second build of the compiler expands macros on the wasm host itself. ASTGen, the
compiler's swift-syntax and SwiftCompilerSources are compiled for `wasm32-wasip1` and
linked into `swift-frontend.wasm`; macro plugins are wasm modules of their own, which the
compiler reaches through its in-process plugin hook.

* **The plugin channel.** With no `dlopen`, `PluginRegistry` (patch `0003`) sends each
  plugin-server message to two wasm imports, `swift_plugin_host.handle_message` and
  `swift_plugin_host.take_response`. The embedder answers the server's own messages
  (`getCapability`, `loadPluginLibrary`) and forwards each expansion to the plugin that
  provides the macro's module, in the same JSON protocol (version 7) the native in-process
  server speaks. [`tools/plugin-host/swift-plugin-host.mjs`](../tools/plugin-host/swift-plugin-host.mjs)
  is that embedder: synchronous, dependency-free, the same code in Node and a browser
  worker. [`run-frontend.mjs`](../tools/plugin-host/run-frontend.mjs) runs the compiler
  under Node with it.
* **The plugins.** Stage 37 builds `SwiftMacroPlugins.wasm`, one WASI module with the
  example `StringifyMacros`, `ObservationMacros` and swift-testing's `TestingMacros`,
  sharing one copy of swift-syntax. The compiler is told where they are with
  `-load-resolved-plugin /path/SwiftMacroPlugins.wasm##StringifyMacros,ObservationMacros,TestingMacros`
  (an empty executable path means in-process).
* **Without JavaScript.** A frontend built this way cannot be instantiated without the two
  imports. [`no-plugins.wat`](../tools/plugin-host/no-plugins.wat) is an embedder with no
  plugins for wasmtime (`--preload swift_plugin_host=no-plugins.wat`): everything that uses
  no macros works, and a macro is reported against its use.
* **Tested.** Stage 57 compiles a file using `#stringify`, `@Observable` and
  `@Test`/`#expect` with `swift-frontend.wasm` under Node, links it with `wasm-ld.wasm`,
  runs it under wasmtime and checks its output, including swift-testing's own report of
  the two tests; a second file must fail with the diagnostic its macro emits. The plugin
  host has its own unit tests (`node --test tools/plugin-host/test`).

SIZES_PLACEHOLDER

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
  `SourceKit::WorkQueue::runPendingWork()`. There is no C entry point for that; an
  embedding in C++ calls it directly, as [sourcekit-lsp-lite](#sourcekit-lsp-lite) does.
* **A queued item that takes a `std::mutex` its waiter holds traps** (wasi-libc's
  single-threaded mutex has nothing to wait for). Nothing in the request paths used by
  completion and cursor info is known to do this, but it is the failure to expect.

## sourcekit-lsp-lite

**Status: passes natively against Swift 6.4.1's `libsourcekitdInProc.so`, and linked for
`wasm32-wasip1` and smoke-tested under wasmtime and Node by build-toolchain.yml (Stage 53).** [`tools/sourcekit-lsp-lite`](../tools/sourcekit-lsp-lite)
is a small LSP server in C++20 over sourcekitd's public C API (`sourcekitd.h`, synchronous
`sourcekitd_send_request_sync` only: no blocks, no notification handler). It exists because
SourceKit-LSP is a Swift program with threads, an index store and a build-system layer,
none of which a single-threaded WASI module can host. Stage 36 links it into the same
module as Stage 35's static sourcekitd; Stage 53 runs its LSP tests under wasmtime and
drives the reactor from Node. Both are optional: a failure warns and the toolchain still ships.

### What each LSP request becomes

| LSP | sourcekitd |
| --- | --- |
| `didOpen` / `didChange` / `didClose` | `editor.open` (`syntactic_only`), `editor.replacetext` per incremental change (or one whole-text replace), `editor.close` |
| `publishDiagnostics` | `source.request.diagnostics`, from idle time after edits stop (50 ms natively), for every open file |
| `completion` | `codecomplete` at the start of the word being typed; the server filters by the typed prefix (a prefix, then a case-insensitive prefix, then a subsequence starting with the same letter), ranks, caps at `maxCompletions`, and turns placeholders into snippets |
| `hover` | `cursorinfo`: the annotated declaration and the doc comment, as Markdown |
| `definition` | `cursorinfo`'s `key.filepath` + `key.offset` (or its module name for SDK symbols, which yield nothing) |
| `references` | `cursorinfo` for the USR, then in every project file `cursorinfo` on textual occurrences of the name until one has that USR, then `relatedidents` from there |
| `documentHighlight` | `relatedidents` (base names only: without swift-syntax there is no name matcher for argument labels) |
| `prepareRename` / `rename` | `cursorinfo` for the symbol (refused, with the reason as a `RequestFailed` message, for the standard library, modules, initializers, subscripts and operators), then the occurrences `references` finds, every candidate resolved; each base-name range is checked to still spell the old name before it is rewritten. A new name with argument labels (`moved(to:)`) is refused: renaming labels needs swift-syntax's name matcher, which this build does not have |
| `documentSymbol` | a throwaway `editor.open` with substructure, mapped to a `DocumentSymbol` tree |
| `signatureHelp` | `signaturehelp` |
| `inlayHint` | `variable.type` over the range: `: Type` after each variable whose type is inferred |
| `codeAction` | no request: each published diagnostic carries its fix-its as quick fixes in `codeActions` (clangd's extension, as SourceKit-LSP does), the diagnostic's own as one preferred action titled by its edit, each note's titled by the note; the server returns those of the diagnostics the client sends back, so an edit is always the one computed for the text the client saw |

Positions are UTF-16 code units unless the client offers `utf-8` in
`general.positionEncodings`; a line table converts them to sourcekitd's UTF-8 byte offsets
and back, with surrogate pairs and lone surrogates handled. Every request gets the project's
files as compiler arguments (every `*.swift` under the root, minus hidden directories,
`node_modules`, `build` and `exclude` prefixes), the module name from `@testable import`
(else `main`), and whatever `compilerArgs` the client gives.

Unsaved text: diagnostics and cursor info see other files' editor buffers, but
`codecomplete` and `signaturehelp` read other files from disk. So the server mirrors every
open document into a shadow directory and passes `-vfsoverlay` mapping each real path to
its mirror (`use-external-names: false`). On WASI the shadow directory is created under
`$TMPDIR`, so the host must preopen a writable `/tmp` and set `TMPDIR=/tmp` (or say
`shadowDirectory: "none"` and accept stale completions across files).

### What SourceKit-LSP has that this does not

* **No index.** No index store, no background indexing, no workspace symbols, no call or
  type hierarchy. References are a textual search narrowed by USR, so they cover the
  project's files only, and an initializer, whose uses are spelled as the type's name,
  is found less reliably than a named declaration.
* **No build system.** No SwiftPM, no `compile_commands.json`, no multiple modules: one
  module, the files the server scanned, the arguments the client sent.
* **Rename covers base names only** (above), and there is **no formatting, semantic
  tokens, refactoring actions, folding ranges, generated interfaces, or macro expansion**
  (macros cannot run on WASI anyway).
* **No cancellation and no concurrency.** One request at a time; `$/cancelRequest` is
  ignored, and a slow answer holds the next message.
* **No `completionItem/resolve`.** Items carry their documentation brief up front.

### Driving it from JavaScript

`sourcekit-lsp-lite-reactor.wasm` is a WASI reactor; a browser worker instantiates it once
with a WASI shim and calls `_initialize`. It never reads stdin. Per JSON-RPC message, as
UTF-8 JSON without framing:

```js
const p = lsp.lsp_alloc(bytes.length);
new Uint8Array(lsp.memory.buffer, p, bytes.length).set(bytes);
const n = lsp.lsp_handle(p, bytes.length);   // runs the request to completion
lsp.lsp_free(p);
const out = decoder.decode(new Uint8Array(lsp.memory.buffer, lsp.lsp_output(), n));
// out: zero or more messages, one JSON object per line (replies and notifications);
// read it before the next call, which overwrites it.
```

When no message is waiting and `lsp_has_idle_work()` is nonzero, call `lsp_idle()` (same
output contract) from a debounced timer: that is where diagnostics are computed and
published, one file per call so typing stays responsive. `lsp_exit_code()` is -1 until
`exit`, then 0 or 1. The preopens the worker needs: the sysroot, the project at the path its
URIs name, and a writable `/tmp`. The compiler arguments (`-target wasm32-unknown-wasip1`,
`-sdk`, `-resource-dir`, `-module-cache-path`) go in `initializationOptions.compilerArgs`.
[`tests/integration/reactor_host.mjs`](../tools/sourcekit-lsp-lite/tests/integration/reactor_host.mjs)
is the reference host; `sourcekit-lsp-lite.wasm` (a command, LSP on stdio with
`Content-Length` framing) is the same server for wasmtime and tests.

### One module for the compiler and the server

`swift-frontend-lsp.wasm` is the reactor above plus one more export,
`swift_frontend_main(argc, argv)` ([`src/frontend.cpp`](../tools/sourcekit-lsp-lite/src/frontend.cpp)),
which does what `swift-frontend` does with those arguments (`argv[0]` is
`"swift-frontend"`, strings and the pointer array in linear memory from `lsp_alloc`). Stage
36 links it from sourcekitd-request's link command plus the libraries swift-frontend's adds
(`libswiftDriverTool` and what it needs), so the compiler's code, most of both modules, is
there once: a page that both edits and compiles Swift downloads and compiles one module
instead of `swift-frontend.wasm` and `sourcekit-lsp-lite-reactor.wasm`. Stage 36 prints the
three modules' sizes.

The host instantiates the compiled `WebAssembly.Module` once for the language server and
again for **each** compile: `swift_frontend_main` runs the compiler as a process would
(its `InitLLVM` shuts LLVM down on return), so an instance that compiled is spent, for
compiling and for `lsp_*` alike. Instantiating is cheap next to compiling the module. A
fatal compiler error that calls `exit` arrives as WASI `proc_exit`. `reactor_host.mjs
--compile` does exactly this, and Stage 53 runs it.

The server calls `SourceKit::WorkQueue::runPendingWork()` after every message and idle
step (`src/pump.cpp`, linked against the patch's C++ declaration), so queued sourcekitd work
does not pile up between requests; no C entry point was needed.

### Native build and checks

`cmake --preset <name> && cmake --build --preset <name> && ctest --preset <name>`, with
`SOURCEKITD_PREFIX` set to a Swift toolchain's `usr`. Presets: `dev` (clang, the warnings in
`cmake/warnings.txt` as errors), `gcc` (GCC plus its extra warnings), `asan` (Address,
Leak and UndefinedBehavior sanitizers), `tidy` (clang-tidy as errors during the build,
`.clang-tidy`), `cppcheck`, and `release`. ctest runs the doctest unit tests (JSON, text
and position conversion, framing, options, request building, and translation of recorded
sourcekitd responses in `tests/fixtures`), the reactor driven in process, and sixteen LSP
scenarios over stdio against `tests/project`. [`lsp-lite.yml`](../.github/workflows/lsp-lite.yml)
runs all of it, plus a wasm compile with the same warnings.

## Known limits

* **Macros need the macros build** (above). The default build has no swift-syntax, so a
  file that uses a macro fails to compile with it.
* **No Swift-implemented SIL optimizer passes in the default build.**
  `SWIFT_ENABLE_SWIFT_IN_SWIFT` is off there; the macros build turns it on. The compiler's
  Swift code needs C++ interop, which the stock Swift 6.3.3 SDK breaks for
  `wasm32-unknown-wasip1` with a Clang module cycle, `SwiftWASILibc -> std_inttypes_h ->
  SwiftWASILibc`; the macros build installs a fixed `wasi-libc.modulemap` (patch `0002`, a
  port of [swift#90332]; see [upstream-status.md](upstream-status.md)).
* **4 GiB of address space.** Single-file compiles fit comfortably; whole-module builds of
  large packages may not.
* **Size.** ~300 MiB of artifacts on a cold load, before compression.

[swift#90332]: https://github.com/swiftlang/swift/pull/90332
