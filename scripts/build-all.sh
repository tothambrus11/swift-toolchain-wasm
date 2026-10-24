#!/usr/bin/env bash
# Run the whole pipeline, in order, from nothing to publishable artifacts.
#
# Every stage is idempotent and caches its work in build/, so re-running after an
# interruption continues rather than starting over. That property is what makes the
# GitHub Actions workflow viable at all: the LLVM build does not fit in one job.
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

STAGES=(
  00-fetch-sources
  05-apply-patches
  # Needs only the sources and the native toolchain, and takes minutes where the
  # stages after it take hours, so it runs first and fails early.
  37-macro-plugins
  10-host-tools
  15-cmark-wasm
  20-llvm-wasm
  30-swift-frontend-wasm
  35-sourcekitd-wasm
  36-sourcekit-lsp-lite-wasm
  40-sysroot-pack
  55-check-alignment-contracts
)

# Stages whose failure is reported and does not stop the pipeline: they are
# new, and nothing after them depends on them. The workflow checks their
# artifacts separately.
OPTIONAL_STAGES=(36-sourcekit-lsp-lite-wasm)

# Optionally stop early, e.g. BUILD_UNTIL=20-llvm-wasm ./scripts/build-all.sh
: "${BUILD_UNTIL:=}"

for stage in "${STAGES[@]}"; do
  log "=== ${stage} ==="
  if [[ " ${OPTIONAL_STAGES[*]} " == *" ${stage} "* ]]; then
    "${TOOLCHAIN_ROOT}/scripts/${stage}.sh" || warn "${stage} failed; continuing without its artifacts"
  else
    "${TOOLCHAIN_ROOT}/scripts/${stage}.sh"
  fi
  [[ -n "$BUILD_UNTIL" && "$stage" == "$BUILD_UNTIL" ]] && { log "stopping after ${stage}"; break; }
done

log "artifacts in ${TC_OUT}:"
ls -la "$TC_OUT"
