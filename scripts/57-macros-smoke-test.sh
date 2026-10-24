#!/usr/bin/env bash
# Stage 57 — prove a macro-enabled toolchain (SWIFT_MACROS=1) expands real macros.
#
# Like Stage 50, everything that compiles or links is wasm: swift-frontend.wasm,
# run under Node by tools/plugin-host/run-frontend.mjs, which is the embedder
# that runs the macro plugins (SwiftMacroPlugins.wasm from Stage 37) when the
# compiler asks; then wasm-ld.wasm; then the program, under wasmtime. One file
# uses a freestanding expression macro of its own (#stringify), @Observable
# from Observation, and swift-testing's @Test and #expect, whose tests the
# program then runs. A second file must fail to compile with the diagnostic
# its macro emits, which is the plugin's diagnostics reaching the compiler.
#
# The sysroot is the whole Swift SDK, not Stage 40's core tar: swift-testing's
# library needs FoundationEssentials (its JSON coding) and autolinks Foundation,
# which the core profile leaves out. It is mounted where the core tar would put
# it, so the commands are the ones a browser runs.
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

: "${NATIVE_SWIFT_BIN:=${HOME}/.local/share/swiftly/toolchains/${SWIFT_VERSION}/usr/bin}"

WORK="${TC_BUILD}/macros-smoke"
RUN_FRONTEND="${TOOLCHAIN_ROOT}/tools/plugin-host/run-frontend.mjs"

for artifact in swift-frontend.wasm wasm-ld.wasm SwiftMacroPlugins.wasm; do
  [[ -f "${TC_OUT}/${artifact}" ]] || die "missing ${TC_OUT}/${artifact}"
done
command -v node >/dev/null || die "the macro smoke test needs Node.js"
command -v wasmtime >/dev/null || die "the macro smoke test needs wasmtime"
[[ -x "${NATIVE_SWIFT_BIN}/swift-autolink-extract" ]] ||
  die "no swift-autolink-extract at ${NATIVE_SWIFT_BIN}"

source "${TOOLCHAIN_ROOT}/scripts/wasm-sdk.sh"
SWIFT_MACROS=1 fetch_wasm_sdk
WASI_SYSROOT="${WASM_SDK_BUNDLE}/WASI.sdk"
SWIFT_STATIC="${WASM_SDK_BUNDLE}/swift.xctoolchain/usr/lib/swift_static"
STATIC_LIBS="${SWIFT_STATIC}/wasi"
[[ -f "${STATIC_LIBS}/libTesting.a" && -d "${STATIC_LIBS}/Observation.swiftmodule" ]] ||
  die "the Swift SDK has no Testing or Observation"

rm -rf "$WORK"
mkdir -p "${WORK}/src" "${WORK}/build" "${WORK}/mcache" "${WORK}/plugins"
cp "${TC_OUT}/SwiftMacroPlugins.wasm" "${WORK}/plugins/"
# Preopens, in wasmtime's and run-frontend.mjs's HOST::GUEST form.
MOUNTS=(--dir "${WORK}::/work"
        --dir "${WASI_SYSROOT}::/sysroot/wasi-sysroot"
        --dir "${SWIFT_STATIC}::/sysroot/swift/lib/swift_static")

cat > "${WORK}/src/main.swift" <<'SWIFT'
import Observation
import Testing

@freestanding(expression)
macro stringify<T>(_ value: T) -> (T, String) =
  #externalMacro(module: "StringifyMacros", type: "StringifyMacro")

@Observable
final class Counter {
  var count = 0
}

@Test func arithmeticStillWorks() {
  #expect(1 + 1 == 2)
}

@Test func anObservableCounterCounts() {
  let counter = Counter()
  counter.count += 2
  #expect(counter.count == 2)
}

let (value, source) = #stringify(2 + 3)
print("stringify: \(value) from \(source)")

let counter = Counter()
withObservationTracking {
  _ = counter.count
} onChange: {
  print("observed: count is changing")
}
counter.count += 1

let status: CInt = await Testing.__swiftPMEntryPoint(passing: nil)
print("tests exited with \(status)")
SWIFT

cat > "${WORK}/src/diagnose.swift" <<'SWIFT'
@freestanding(expression)
macro diagnose(_ message: String) =
  #externalMacro(module: "StringifyMacros", type: "DiagnoseMacro")

#diagnose("reported by the plugin")
SWIFT

compile() {
  local source="$1" object="$2"
  node "$RUN_FRONTEND" "${MOUNTS[@]}" "${TC_OUT}/swift-frontend.wasm" \
    -frontend -c -primary-file "/work/src/${source}" \
    -target wasm32-unknown-wasip1 -disable-objc-interop \
    -sdk /sysroot/wasi-sysroot \
    -resource-dir /sysroot/swift/lib/swift_static -use-static-resource-dir \
    -module-cache-path /work/mcache \
    -Xcc -D_WASI_EMULATED_SIGNAL -Xcc -D_WASI_EMULATED_MMAN \
    -load-resolved-plugin "/work/plugins/SwiftMacroPlugins.wasm##StringifyMacros,ObservationMacros,TestingMacros" \
    -no-color-diagnostics -empty-abi-descriptor \
    -module-name main -o "/work/build/${object}"
}

log "compiling a file that uses #stringify, @Observable, @Test and #expect"
compile main.swift main.o
[[ -f "${WORK}/build/main.o" ]] || die "swift-frontend.wasm produced no object file"

log "checking that a macro's diagnostics reach the compiler"
if compile diagnose.swift diagnose.o 2> "${WORK}/diagnose.log"; then
  die "a file whose macro emits an error compiled"
fi
grep -q "error: reported by the plugin" "${WORK}/diagnose.log" || {
  cat "${WORK}/diagnose.log" >&2
  die "the compiler did not report the plugin's diagnostic"
}

# The libraries main.o needs, and theirs: the autolink entries Swift objects
# carry, followed through the static archives they name until nothing new
# appears. That is what swiftc's driver does on a native host.
libraries=()
pending=("${WORK}/build/main.o")
while (( ${#pending[@]} > 0 )); do
  next=()
  for flag in $("${NATIVE_SWIFT_BIN}/swift-autolink-extract" "${pending[@]}"); do
    name="${flag#-l}"
    [[ " ${libraries[*]} " == *" ${name} "* ]] && continue
    libraries+=("$name")
    [[ -f "${STATIC_LIBS}/lib${name}.a" ]] && next+=("${STATIC_LIBS}/lib${name}.a")
  done
  pending=("${next[@]}")
done
log "linking with wasm-ld.wasm against: ${libraries[*]}"

link_flags=()
for name in "${libraries[@]}"; do link_flags+=("-l${name}"); done
wasmtime run "${MOUNTS[@]}" "${TC_OUT}/wasm-ld.wasm" \
  -m wasm32 \
  -L/sysroot/swift/lib/swift_static/wasi \
  -L/sysroot/wasi-sysroot/lib/wasm32-wasip1 \
  /sysroot/wasi-sysroot/lib/wasm32-wasip1/crt1-command.o \
  /sysroot/swift/lib/swift_static/wasi/wasm32/swiftrt.o \
  /work/build/main.o \
  "${link_flags[@]}" \
  -lswiftSwiftOnoneSupport -ldl -lc++ -lc++abi -lm \
  -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks \
  --error-limit=0 --threads=1 --global-base=4096 --table-base=4096 \
  -z stack-size=1048576 --gc-sections -lc \
  /sysroot/swift/lib/swift_static/clang/lib/wasip1/libclang_rt.builtins-wasm32.a \
  -o /work/build/program.wasm
[[ -f "${WORK}/build/program.wasm" ]] || die "wasm-ld.wasm produced no program"

log "running the program and its tests"
output="$(wasmtime run "${WORK}/build/program.wasm")" || {
  printf '%s\n' "$output" >&2
  die "the program failed"
}
printf '%s\n' "$output"

expect_line() {
  grep -qF -- "$1" <<<"$output" || die "the program did not print '$1'"
}
expect_line "stringify: 5 from 2 + 3"
expect_line "observed: count is changing"
expect_line "Test run with 2 tests"
expect_line "tests exited with 0"

log "macro smoke test passed: #stringify, @Observable, @Test and #expect expanded by wasm plugins"
