#!/usr/bin/env bash
# Shared configuration for the swift-toolchain-wasm build pipeline.
# Source this; do not execute it.

set -euo pipefail

# --- versions -----------------------------------------------------------------
# The Swift toolchain and the Swift SDK for WebAssembly must match exactly
# (enforced from Swift 6.1 onward), and LLVM is pinned to the same release so the
# wasm-host compiler we build is the same compiler users get natively.
: "${SWIFT_VERSION:=6.3.3}"
: "${SWIFT_TAG:=swift-${SWIFT_VERSION}-RELEASE}"
: "${WASI_SDK_VERSION:=34.0}"
: "${WASI_SDK_MAJOR:=${WASI_SDK_VERSION%%.*}}"

# The wasm SDK artifact bundle checksum, from swift.org's install instructions.
: "${SWIFT_WASM_SDK_URL:=https://download.swift.org/swift-${SWIFT_VERSION}-release/wasm-sdk/${SWIFT_TAG}/${SWIFT_TAG}_wasm.artifactbundle.tar.gz}"
: "${SWIFT_WASM_SDK_CHECKSUM:=cabfa08b73bb8ac783927ecd15fa386e99d0c139c5f232445067bcf58379cae7}"

# --- layout -------------------------------------------------------------------
TOOLCHAIN_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${TC_SRC:=${TOOLCHAIN_ROOT}/src}"     # third-party sources (gitignored)
: "${TC_BUILD:=${TOOLCHAIN_ROOT}/build}" # intermediate build trees (gitignored)
: "${TC_OUT:=${TOOLCHAIN_ROOT}/out}"     # shippable .wasm artifacts (gitignored)
: "${WASI_SDK_PATH:=${TC_SRC}/wasi-sdk-${WASI_SDK_VERSION}-x86_64-linux}"
: "${LLVM_SRC:=${TC_SRC}/llvm-project}"

# --- target -------------------------------------------------------------------
# wasip1 (not the threads variant) for the compiler itself: single-threaded keeps the
# build tractable and avoids SharedArrayBuffer/COOP-COEP requirements on the page.
: "${WASM_TARGET:=wasm32-wasip1}"
: "${JOBS:=$(nproc)}"

mkdir -p "$TC_SRC" "$TC_BUILD" "$TC_OUT"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33m warning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m error:\033[0m %s\n' "$*" >&2; exit 1; }
