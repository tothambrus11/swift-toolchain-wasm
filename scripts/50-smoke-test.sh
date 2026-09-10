#!/usr/bin/env bash
# Stage 50 — prove the produced artifacts actually work.
#
# Compiles and links a real Swift program using ONLY the wasm tools, under a standalone
# WASI runtime, then runs the program the same way. This is the check that matters: the
# artifacts are useless if they cannot do this, and a browser will do exactly this.
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

WORK="${TC_BUILD}/smoke"
SYSROOT="${WORK}/sysroot"

for artifact in swift-frontend.wasm wasm-ld.wasm swift-sysroot-core.tar; do
  [[ -f "${TC_OUT}/${artifact}" ]] || die "missing ${TC_OUT}/${artifact}"
done

runtime=""
for candidate in wasmtime wasmer; do
  command -v "$candidate" >/dev/null && { runtime="$candidate"; break; }
done
[[ -n "$runtime" ]] || die "no WASI runtime found (install wasmtime)"

rm -rf "$WORK"
mkdir -p "${SYSROOT}" "${WORK}/src" "${WORK}/build"
tar -xf "${TC_OUT}/swift-sysroot-core.tar" -C "$SYSROOT"

cat > "${WORK}/src/main.swift" <<'SWIFT'
let squares = (1...5).map { $0 * $0 }
print("squares: \(squares)")
SWIFT

# The tools see one directory tree, exactly as they will in the browser's VirtualFS.
run_tool() {
  local tool="$1"; shift
  "$runtime" run --dir "${WORK}::/work" --dir "${SYSROOT}::/sysroot" "${TC_OUT}/${tool}" -- "$@"
}

log "compiling with swift-frontend.wasm"
run_tool swift-frontend.wasm \
  -frontend -c -primary-file /work/src/main.swift \
  -target wasm32-unknown-wasip1 -disable-objc-interop \
  -sdk /sysroot/wasi-sysroot \
  -resource-dir /sysroot/swift/lib/swift_static -use-static-resource-dir \
  -no-color-diagnostics -empty-abi-descriptor \
  -module-name main -o /work/build/main.o

[[ -f "${WORK}/build/main.o" ]] || die "swift-frontend.wasm produced no object file"

log "linking with wasm-ld.wasm"
run_tool wasm-ld.wasm \
  -m wasm32 \
  -L/sysroot/swift/lib/swift_static/wasi \
  -L/sysroot/wasi-sysroot/lib/wasm32-wasip1 \
  /sysroot/wasi-sysroot/lib/wasm32-wasip1/crt1-command.o \
  /sysroot/swift/lib/swift_static/wasi/wasm32/swiftrt.o \
  /work/build/main.o \
  -lswiftSwiftOnoneSupport -lswiftCore -lswift_Concurrency -lswift_StringProcessing \
  -lswift_RegexParser -ldl -lc++ -lc++abi -lm \
  -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks \
  --error-limit=0 --threads=1 --global-base=4096 --table-base=4096 \
  -z stack-size=131072 -lc \
  /sysroot/swift/lib/swift_static/clang/lib/wasip1/libclang_rt.builtins-wasm32.a \
  -o /work/build/program.wasm

[[ -f "${WORK}/build/program.wasm" ]] || die "wasm-ld.wasm produced no program"

log "running the compiled program"
actual="$("$runtime" run "${WORK}/build/program.wasm")"
expected="squares: [1, 4, 9, 16, 25]"

if [[ "$actual" != "$expected" ]]; then
  die "program printed '${actual}', expected '${expected}'"
fi

log "smoke test passed: Swift compiled, linked and run entirely by wasm"
