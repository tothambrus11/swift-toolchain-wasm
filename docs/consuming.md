# Consuming the artifacts from a web app

The artifacts are plain static files, published on the
[`swift-6.3.3-wasm` release](https://github.com/tothambrus11/swift-toolchain-wasm/releases/tag/swift-6.3.3-wasm)
alongside a `.gz` of each — 69 MiB over the wire instead of 302 MiB. `latest` follows a
rebuild of the same Swift release:

```
https://github.com/tothambrus11/swift-toolchain-wasm/releases/latest/download/swift-frontend.wasm.gz
```

Release assets carry no `Access-Control-Allow-Origin` and are served as
`application/octet-stream`, so a page cannot fetch them directly and
`WebAssembly.compileStreaming` would reject them anyway. Proxy them through your own
origin, setting `application/wasm` yourself; `packages/ide/src/worker.js` in the Yukibana
repository is one such proxy, in about sixty lines.

A page needs three of them and a way to run WASI command modules over a shared in-memory
filesystem.

```
/toolchain/swift-frontend.wasm
/toolchain/wasm-ld.wasm
/toolchain/swift-sysroot-core.tar
```

## The pipeline a browser runs

There is no driver — WASI cannot spawn processes — so the embedder replays what the driver
would have done. These argument vectors are captured from a real
`swiftc -target wasm32-unknown-wasip1 -v` run, then verified by replaying them by hand:

```
swift-frontend -frontend -c <sources...> -primary-file <one of them>
  -target wasm32-unknown-wasip1
  -disable-objc-interop
  -sdk        /sysroot/wasi-sysroot
  -resource-dir /sysroot/swift/lib/swift_static -use-static-resource-dir
  -no-color-diagnostics -empty-abi-descriptor
  -module-name main -o /build/main.o

wasm-ld -m wasm32
  -L/sysroot/swift/lib/swift_static/wasi
  -L/sysroot/wasi-sysroot/lib/wasm32-wasip1
  /sysroot/wasi-sysroot/lib/wasm32-wasip1/crt1-command.o
  /sysroot/swift/lib/swift_static/wasi/wasm32/swiftrt.o
  /build/main.o
  -lswiftSwiftOnoneSupport -lswiftCore -lswift_Concurrency -lswift_StringProcessing
  -lswift_RegexParser -ldl -lc++ -lc++abi -lm
  -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks
  --error-limit=0 --threads=1 --global-base=4096 --table-base=4096
  -z stack-size=131072 -lc
  /sysroot/swift/lib/swift_static/clang/lib/wasip1/libclang_rt.builtins-wasm32.a
  -o /build/program.wasm
```

Two steps the driver takes that an embedder should skip:

* **`swift-autolink-extract`.** It reads the object's autolink section to decide which
  libraries to link. For a wasm target the answer is the fixed list above.
* **`-plugin-path` / `-in-process-plugin-server-path`.** Macro plumbing, and macros cannot
  work here at all (see [porting.md](porting.md)).

One detail that is easy to get wrong: every source is listed **once**, with `-primary-file`
marking the one being compiled. Listing the primary twice is a duplicate-input error.

## Practical notes

* Unpack `swift-sysroot-core.tar` into the virtual filesystem at `/sysroot` **once** per
  session and reuse it; it is ~99 MiB unpacked.
* `WebAssembly.compile` the tools once per tab. For modules this size, compilation
  dominates a rebuild.
* Serve `.wasm` and `.tar` pre-compressed with a long `Cache-Control`; the first load is
  the only expensive one.
* Run compilation in a worker. A compile takes long enough to jank the UI thread.
