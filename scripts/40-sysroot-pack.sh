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

# Get the wasm SDK bundle ourselves rather than hunting for what `swift sdk install`
# did with it.
#
# Three CI runs died here with "wasm SDK not installed" while `swift sdk list` printed
# the SDK happily in the very next line — SwiftPM knows where it put the bundle, and
# every guess at that path (two locations, then four, then following symlinks) was
# wrong on the runner and right on my machine. The bundle is a plain tarball at a URL
# this repository already pins, with a checksum it already knows, so downloading and
# extracting it is deterministic and removes the guessing entirely. It lands in the
# cached sources directory, so CI pays for it once.
SDK_ROOT="${TC_SRC}/swift-wasm-sdk"

if [[ -z "${SDK_BUNDLE:-}" ]]; then
  if [[ ! -d "$SDK_ROOT" ]]; then
    log "downloading the wasm SDK bundle (${SWIFT_TAG})"
    tmp="$(mktemp -d)"
    curl -fsSL --retry 3 -o "${tmp}/wasm-sdk.tar.gz" "$SWIFT_WASM_SDK_URL"
    actual="$(sha256sum "${tmp}/wasm-sdk.tar.gz" | cut -d' ' -f1)"
    if [[ "$actual" != "$SWIFT_WASM_SDK_CHECKSUM" ]]; then
      rm -rf "$tmp"
      die "wasm SDK checksum mismatch: expected ${SWIFT_WASM_SDK_CHECKSUM}, got ${actual}"
    fi
    mkdir -p "$SDK_ROOT"
    tar -xzf "${tmp}/wasm-sdk.tar.gz" -C "$SDK_ROOT"
    rm -rf "$tmp"
  fi
  SDK_BUNDLE="$(dirname "$(find -L "$SDK_ROOT" -maxdepth 5 -type d -name WASI.sdk -print -quit)")"
fi

STAGE="${TC_BUILD}/sysroot"

if [[ -z "${SDK_BUNDLE:-}" || ! -d "${SDK_BUNDLE}/WASI.sdk" ]]; then
  warn "no WASI.sdk under ${SDK_ROOT}; its contents are:"
  find -L "$SDK_ROOT" -maxdepth 3 >&2 2>/dev/null || true
  die "wasm SDK bundle did not contain a WASI.sdk directory"
fi
log "using wasm SDK at ${SDK_BUNDLE}"

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
