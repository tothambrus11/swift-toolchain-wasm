# Porting notes: what a wasm host takes away

A wasm host is a POSIX-shaped environment with the process model removed. Almost every
patch in this repository follows from that one fact, and knowing it up front saves
rediscovering it one compile error at a time.

## What WASI does not have

| Absent | Consequence |
| --- | --- |
| `fork`, `exec`, `posix_spawn`, `wait` | No subprocesses. Compiler drivers, plugin servers and anything that shells out cannot work. |
| Signals (`sigaction`, `siginfo_t`, `kill`, `alarm`) | No crash handlers, no interrupt handling, no watchdogs. |
| `setjmp`/`longjmp` (without the EH proposal) | LLVM's crash recovery cannot unwind — and a wasm trap tears the instance down anyway. |
| `getrlimit`/`setrlimit` | No stack or memory limits to query; the linker's `-z stack-size` sets the stack instead. |
| The user database (`pwd.h`, `getpwuid`) | No home directory beyond `$HOME`, no `~user` expansion. |
| Unix domain sockets (`sockaddr_un.sun_path`) | Nothing can listen; dependency-scanning daemons are out. |
| `dladdr`, `/proc` | A module cannot find its own path; `argv[0]` is all there is. |
| Terminal ioctls, `umask`, `fchown` | No terminal width, no permission mask, no file ownership. |

## Consequences for the design

**Drive the frontend in-process, never the driver.** `swiftc` is a driver that spawns
`swift-frontend` and a linker. Under WASI it can do neither, so the embedder invokes the
frontend directly with an explicit argument vector, then invokes `wasm-ld` as a separate
module. The two share a filesystem, which the embedder provides.

**Report absence, do not fake it.** `llvm/lib/Support/WASI/Program.inc` returns an error
saying WASI cannot launch processes rather than silently succeeding. A caller that needs a
subprocess then fails where the problem is, instead of somewhere later and stranger.

**One libc++ for the whole toolchain.** wasi-sdk's `wasm32-wasip1` libc++ sets
`_LIBCPP_HAS_THREADS=1`; the Swift SDK's `WASI.sdk` sets it to `0`. Mixing them gives
either a compile error (`llvm/Support/Mutex.h` needs `std::recursive_mutex`) or, worse,
two libc++ ABIs in one binary. This repository uses wasi-sdk's for all C++, and takes only
the Swift side — stdlib modules and archives — from the Swift SDK.

**Cross builds need native TableGen first.** LLVM and clang generate much of their own
source with `llvm-tblgen` and `clang-tblgen`, which must run on the build machine. Swift
additionally demands them explicitly (`LLVM_TABLEGEN`, `CLANG_TABLEGEN`) or it insists on
an `${LLVM_BINARY_DIR}/NATIVE` directory that a cross build has no reason to have.

## Limits to plan around

* **4 GiB of address space.** wasm32 caps a module's memory. Single-file compiles fit;
  whole-module builds of large packages may not. An embedder should keep a server-side
  fallback for those.
* **Module size.** These are tens of megabytes of wasm. Serve them compressed, cache them
  (Origin Private FS or the Cache API), and compile the `WebAssembly.Module` once per tab.
* **No macros.** Until plugins are redesigned as in-process wasm modules, any source using
  macros will not compile in the browser.
