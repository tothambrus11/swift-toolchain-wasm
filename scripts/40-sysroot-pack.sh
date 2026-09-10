#!/usr/bin/env bash
# Stage 40 — pack the Swift SDK for WebAssembly into the sysroot the browser mounts.
#
# The in-browser compiler needs the same files a native cross-compile needs: the WASI
# sysroot (libc, crt1, headers) and Swift's static resource directory (.swiftmodule
# interfaces, .a archives, swiftrt.o, compiler-rt). Rather than thousands of fetches,
# they ship as one tar the VirtualFS unpacks — the layout below is exactly what the
# verified argv in packages/runtime/src/pipeline.ts expects.
#
# The paths were not invented: they were read off a real `swiftc -v` invocation for
# wasm32-unknown-wasip1, and the resulting two-step frontend+wasm-ld pipeline was run
# by hand to confirm it links and the program runs.
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

: "${SDK_BUNDLE:=${HOME}/.swiftpm/swift-sdks/${SWIFT_TAG}_wasm.artifactbundle/${SWIFT_TAG}_wasm/wasm32-unknown-wasip1}"
STAGE="${TC_BUILD}/sysroot"

[[ -d "${SDK_BUNDLE}/WASI.sdk" ]] ||
  die "wasm SDK not installed; see docs/pipeline.md (swift sdk install …)"

rm -rf "$STAGE"
mkdir -p "${STAGE}/wasi-sysroot" "${STAGE}/swift/lib"

log "copying the WASI sysroot (libc, crt1, headers)"
cp -a "${SDK_BUNDLE}/WASI.sdk/." "${STAGE}/wasi-sysroot/"

log "copying Swift's static resource directory (stdlib modules and archives)"
cp -a "${SDK_BUNDLE}/swift.xctoolchain/usr/lib/swift_static" "${STAGE}/swift/lib/"

# What to keep. The full stdlib is 184 MiB, most of it Foundation — and 40 MiB of that
# is ICU data alone — which is a lot to push through a browser before the first compile.
#   core (default): Swift stdlib, concurrency, regex. No Foundation, no XCTest.
#   full:           everything the SDK ships.
: "${SYSROOT_PROFILE:=core}"

prune() {
  find "${STAGE}/swift/lib/swift_static/wasi" -maxdepth 1 \( "$@" \) \
    -exec rm -rf {} + 2>/dev/null || true
}

# Test frameworks are never usable in the playground, whatever the profile.
prune -name 'XCTest.*' -o -name 'libXCTest*' -o -name 'Testing.*' -o -name 'libTesting*' \
      -o -name '*.swiftcrossimport'

case "$SYSROOT_PROFILE" in
  core)
    log "pruning Foundation (profile: core)"
    prune -name 'Foundation*' -o -name 'libFoundation*' -o -name '_Foundation*' \
          -o -name 'lib_Foundation*'
    ;;
  full) log "keeping the complete stdlib (profile: full)" ;;
  *) die "unknown SYSROOT_PROFILE '${SYSROOT_PROFILE}' (expected core or full)" ;;
esac

OUT_TAR="${TC_OUT}/swift-sysroot-${SYSROOT_PROFILE}.tar"
log "packing"
tar -cf "$OUT_TAR" -C "$STAGE" .
gzip -9 -kf "$OUT_TAR"

printf 'wrote %s (%s, %s gzipped)\n' \
  "$OUT_TAR" "$(du -h "$OUT_TAR" | cut -f1)" "$(du -h "${OUT_TAR}.gz" | cut -f1)"
