#!/usr/bin/env bash
# Stage 35 — cross-build `swift-ide-test` for a wasm host.
#
# swift-ide-test drives the same IDE-facing code (code completion, cursor info,
# module/AST printing) that SourceKit exposes to editors, but as a standalone CLI —
# useful for exercising completion, etc. without a SourceKit round-trip. It links
# against swiftIDE/swiftIDETool/swiftAST/swiftFrontend, all of which Stage 30 already
# built to produce swift-frontend.wasm, so this stage is a second `ninja` target against
# the same configured build tree, not a second configure-and-compile pass.
#
# Requires Stage 30's cmake configure to have run with SWIFT_INCLUDE_TEST_BINARIES=ON —
# that flag is what makes CMake add tools/swift-ide-test as a subdirectory at all.
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

SWIFT_BUILD="${TC_BUILD}/wasm-swift"

[[ -f "${SWIFT_BUILD}/build.ninja" ]] || die "run 30-swift-frontend-wasm.sh first"

log "building swift-ide-test"
ninja -C "$SWIFT_BUILD" -j "$JOBS" swift-ide-test

install -D "${SWIFT_BUILD}/bin/swift-ide-test" "${TC_OUT}/swift-ide-test.wasm"
log "wrote ${TC_OUT}/swift-ide-test.wasm ($(du -h "${TC_OUT}/swift-ide-test.wasm" | cut -f1))"
