#!/usr/bin/env bash
# Stage 00 — fetch the pipeline's inputs: wasi-sdk (the C/C++ cross toolchain that
# targets a wasm host) and the swiftlang LLVM sources the compiler is built from.
# Idempotent: anything already present is left alone.
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

if [[ -x "${WASI_SDK_PATH}/bin/clang" ]]; then
  log "wasi-sdk ${WASI_SDK_VERSION} already present"
else
  log "fetching wasi-sdk ${WASI_SDK_VERSION}"
  url="https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_SDK_MAJOR}/wasi-sdk-${WASI_SDK_VERSION}-x86_64-linux.tar.gz"
  curl -fsSL "$url" | tar -xz -C "$TC_SRC"
fi

if [[ -d "${LLVM_SRC}/llvm" ]]; then
  log "llvm-project already present at ${LLVM_SRC}"
else
  log "cloning swiftlang/llvm-project @ ${SWIFT_TAG} (shallow)"
  git clone --depth 1 --branch "$SWIFT_TAG" --single-branch \
    https://github.com/swiftlang/llvm-project.git "$LLVM_SRC"
fi

# Stage 30 inputs. Fetched here so the whole pipeline has one entry point, even though
# the swift-frontend build is not yet proven.
if [[ -d "${TC_SRC}/swift/lib" ]]; then
  log "swift already present"
else
  log "cloning swiftlang/swift @ ${SWIFT_TAG} (shallow)"
  git clone --depth 1 --branch "$SWIFT_TAG" --single-branch \
    https://github.com/swiftlang/swift.git "${TC_SRC}/swift"
fi

if [[ -d "${TC_SRC}/swift-cmark/src" ]]; then
  log "swift-cmark already present"
else
  log "cloning swiftlang/swift-cmark @ ${SWIFT_TAG} (shallow)"
  git clone --depth 1 --branch "$SWIFT_TAG" --single-branch \
    https://github.com/swiftlang/swift-cmark.git "${TC_SRC}/swift-cmark"
fi

log "sources ready"
