#!/usr/bin/env bash
# Stage 52 — prove sourcekitd answers on a wasm host.
#
# Runs sourcekitd-request.wasm under a standalone WASI runtime against the same sysroot
# the browser mounts, and asks it the two questions an editor asks most: what can come
# after this dot (source.request.codecomplete), and what is this symbol
# (source.request.cursorinfo). Like Stage 50, no native tool takes part.
#
# The compiler arguments are driver arguments, as an editor would send them;
# sourcekitd turns them into a frontend invocation in process, without spawning
# anything.
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

WORK="${TC_BUILD}/sourcekitd-smoke"
SYSROOT="${WORK}/sysroot"

for artifact in sourcekitd-request.wasm swift-sysroot-core.tar; do
  [[ -f "${TC_OUT}/${artifact}" ]] || die "missing ${TC_OUT}/${artifact}"
done

runtime=""
for candidate in wasmtime wasmer; do
  command -v "$candidate" >/dev/null && { runtime="$candidate"; break; }
done
[[ -n "$runtime" ]] || die "no WASI runtime found (install wasmtime)"

rm -rf "$WORK"
mkdir -p "$SYSROOT" "${WORK}/src" "${WORK}/mcache" "${WORK}/requests"
tar -xf "${TC_OUT}/swift-sysroot-core.tar" -C "$SYSROOT"

SOURCE="${WORK}/src/main.swift"
cat > "$SOURCE" <<'SWIFT'
let squares = (1...5).map { $0 * $0 }
let total = squares.
print(squares)
SWIFT

# Byte offsets: just after "squares." for completion, on the last "squares" for
# cursor info.
read -r complete_offset cursor_offset < <(python3 - "$SOURCE" <<'PY'
import sys
text = open(sys.argv[1], "rb").read()
dot = text.index(b"squares.") + len(b"squares.")
use = text.rindex(b"squares")
print(dot, use)
PY
)

compiler_args='"/work/src/main.swift", "-target", "wasm32-unknown-wasip1",
  "-sdk", "/sysroot/wasi-sysroot",
  "-resource-dir", "/sysroot/swift/lib/swift_static",
  "-module-cache-path", "/work/mcache", "-module-name", "main"'

cat > "${WORK}/requests/complete.yaml" <<YAML
{
  key.request: source.request.codecomplete,
  key.sourcefile: "/work/src/main.swift",
  key.offset: ${complete_offset},
  key.compilerargs: [${compiler_args}]
}
YAML

cat > "${WORK}/requests/cursor.yaml" <<YAML
{
  key.request: source.request.cursorinfo,
  key.sourcefile: "/work/src/main.swift",
  key.offset: ${cursor_offset},
  key.compilerargs: [${compiler_args}]
}
YAML

request() {
  "$runtime" run --dir "${WORK}::/work" --dir "${SYSROOT}::/sysroot" \
    "${TC_OUT}/sourcekitd-request.wasm" "$@"
}

log "source.request.codecomplete after 'squares.'"
completion="$(request /work/requests/complete.yaml)" ||
  die "sourcekitd-request failed on codecomplete"
head -40 <<< "$completion"
grep -q 'key.name: "count"' <<< "$completion" ||
  die "completion after 'squares.' did not offer 'count'"
log "completion offers $(grep -c 'key.name:' <<< "$completion") results, 'count' among them"

log "source.request.cursorinfo on 'squares'"
cursor="$(request /work/requests/cursor.yaml)" ||
  die "sourcekitd-request failed on cursorinfo"
printf '%s\n' "$cursor"
grep -q 'key.name: "squares"' <<< "$cursor" ||
  die "cursor info did not name 'squares'"
grep -q 'key.typename: "\[Int\]"' <<< "$cursor" ||
  die "cursor info did not give 'squares' the type [Int]"

log "sourcekitd smoke test passed: completion and cursor info answered entirely by wasm"
