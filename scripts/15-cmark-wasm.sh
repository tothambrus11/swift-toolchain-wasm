#!/usr/bin/env bash
# Stage 15 — cross-build swift-cmark for the wasm host.
#
# swift-frontend links cmark-gfm for documentation comment parsing, and its CMake
# requires the *built* package, not just the sources. It is small, needs no patches, and
# takes under a minute — but the swift configure fails without it.
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

BUILD_DIR="${TC_BUILD}/wasm-cmark"
INSTALL_DIR="${TC_BUILD}/wasm-cmark-install"
: "${SWIFT_CMARK_SRC:=${TC_SRC}/swift-cmark}"

[[ -d "${SWIFT_CMARK_SRC}/src" ]] || die "swift-cmark sources missing; run 00-fetch-sources.sh"

if [[ -f "${INSTALL_DIR}/lib/cmake/cmark-gfm-config.cmake" ]]; then
  log "cmark-gfm already built for ${WASM_TARGET}"
  exit 0
fi

log "configuring swift-cmark for ${WASM_TARGET}"
cmake -G Ninja -S "$SWIFT_CMARK_SRC" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="${WASI_SDK_PATH}/share/cmake/wasi-sdk-p1.cmake" \
  -DWASI_SDK_PREFIX="${WASI_SDK_PATH}" \
  -DUNIX=1 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
  -DCMARK_TESTS=OFF -DCMARK_SHARED=OFF -DCMARK_STATIC=ON -DBUILD_TESTING=OFF

ninja -C "$BUILD_DIR" -j "$JOBS"
ninja -C "$BUILD_DIR" install

log "cmark-gfm installed at ${INSTALL_DIR}"
