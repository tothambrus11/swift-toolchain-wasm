#!/usr/bin/env bash
# Stage 53 — prove sourcekit-lsp-lite answers on a wasm host.
#
# Drives out/sourcekit-lsp-lite.wasm under wasmtime with the LSP integration
# tests the native build runs (tools/sourcekit-lsp-lite/tests/integration),
# against the same sample project and the browser sysroot, and then
# out/sourcekit-lsp-lite-reactor.wasm and out/swift-frontend-lsp.wasm from Node
# the way a browser worker calls them (tests/integration/reactor_host.mjs); the
# combined module also compiles the project with swift_frontend_main, in a
# second instance of the same compiled module. Tests/ is left out: the sysroot
# has no XCTest.
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

LITE="${TOOLCHAIN_ROOT}/tools/sourcekit-lsp-lite"
WORK="${TC_BUILD}/sourcekit-lsp-lite-smoke"
SYSROOT="${WORK}/sysroot"
PROJECT="${WORK}/project"
: "${LITE_TEST_TIMEOUT:=600}" # seconds per answer: a cold wasm sourcekitd is slow to load the stdlib
export LITE_TEST_TIMEOUT

for artifact in sourcekit-lsp-lite.wasm sourcekit-lsp-lite-reactor.wasm swift-frontend-lsp.wasm swift-sysroot-core.tar; do
  [[ -f "${TC_OUT}/${artifact}" ]] || die "missing ${TC_OUT}/${artifact}"
done
command -v wasmtime >/dev/null || die "no wasmtime (install it: https://wasmtime.dev)"

rm -rf "$WORK"
mkdir -p "$SYSROOT" "${WORK}/tmp/mcache"
tar -xf "${TC_OUT}/swift-sysroot-core.tar" -C "$SYSROOT"
cp -R "${LITE}/tests/project" "$PROJECT"

COMPILER_ARGS=(-target wasm32-unknown-wasip1 -sdk /sysroot/wasi-sysroot
  -resource-dir /sysroot/swift/lib/swift_static -module-cache-path /tmp/mcache)

# The project at the same path inside as outside, so URIs need no mapping.
server_cmd="wasmtime run --dir ${PROJECT}::${PROJECT} --dir ${SYSROOT}::/sysroot --dir ${WORK}/tmp::/tmp"
server_cmd+=" --env TMPDIR=/tmp ${TC_OUT}/sourcekit-lsp-lite.wasm --log -- ${COMPILER_ARGS[*]}"

log "LSP scenarios against sourcekit-lsp-lite.wasm under wasmtime"
python3 "${LITE}/tests/integration/lsp_test.py" --server-cmd "$server_cmd" --project "$PROJECT" \
  --exclude-tests --report \
  --only lifecycle --only diagnostics --only completion --only hover --only definition \
  --only references --only highlights --only rename --only symbols --only signature_help --only utf16 ||
  die "sourcekit-lsp-lite.wasm failed LSP scenarios (see above)"

if command -v node >/dev/null; then
  for module in sourcekit-lsp-lite-reactor swift-frontend-lsp; do
    extra=()
    [[ "$module" == swift-frontend-lsp ]] && extra=(--compile)
    log "${module}.wasm driven from JavaScript, one message per call ${extra[*]}"
    node "${LITE}/tests/integration/reactor_host.mjs" "${TC_OUT}/${module}.wasm" \
      --dir "${PROJECT}=${PROJECT}" --dir "/sysroot=${SYSROOT}" --dir "/tmp=${WORK}/tmp" \
      --root "$PROJECT" --file main.swift --file Geometry.swift "${extra[@]}" -- "${COMPILER_ARGS[@]}" ||
      die "${module}.wasm failed (see above)"
  done
else
  warn "no node; not testing the reactors"
fi

log "sourcekit-lsp-lite smoke test passed: LSP answered entirely by wasm"
