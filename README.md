# Prebuilt toolchain artifacts

The wasm-hosted Swift toolchain, gzipped, on an orphan branch so it carries none of the
repository's history and adds nothing to a normal clone.

| File | Compressed | Uncompressed |
| --- | --- | --- |
| `swift-frontend.wasm.gz` | 35 MB | 146 MB |
| `swift-sysroot-core.tar.gz` | 20 MB | 99 MB |
| `wasm-ld.wasm.gz` | 14 MB | 57 MB |

They are served to the IDE through its Cloudflare Worker, which fetches them from
`raw.githubusercontent.com` and sets `Content-Encoding: gzip` plus the right content
type, so the browser decompresses them transparently and
`WebAssembly.compileStreaming` accepts them directly. That is 69 MB over the wire per
cold visitor rather than 302 MB.

This branch exists because a GitHub release is the better home for these and the release
build is long; once `build-toolchain.yml` publishes one, `TOOLCHAIN_RELEASE` can point at
`releases/latest/download` instead and this branch can go. The artifacts here are the
ones the test suite passes against, including a Swift program compiled and run entirely
in the browser.
