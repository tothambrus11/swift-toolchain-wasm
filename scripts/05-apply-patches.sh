#!/usr/bin/env bash
# Stage 05 — apply Yukibana's patch series to the checked-out sources.
#
# Two repositories need patches, for two different reasons:
#
#   llvm/  — POSIX facilities LLVM assumes that wasm32-wasip1 simply lacks: processes,
#            signals, sockets, resource limits, setjmp. All guarded on __wasi__ so a
#            native build of the same tree is unaffected.
#   swift/ — build-system assumptions that break when the host stdlib comes prebuilt
#            from an SDK rather than being built in-tree.
#
# They are deliberately small, so they stay upstreamable.
# Idempotent: patches already applied are skipped.
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

apply_series() {
  local name="$1" repo="$2"
  local dir="${TOOLCHAIN_ROOT}/patches/${name}"
  shopt -s nullglob
  local patches=("${dir}"/*.patch)
  [[ ${#patches[@]} -gt 0 ]] || { log "no ${name} patches"; return 0; }
  [[ -d "${repo}/.git" ]] || { warn "skipping ${name}: ${repo} is not a checkout"; return 0; }

  local patch base
  for patch in "${patches[@]}"; do
    base="$(basename "$patch")"
    if git -C "$repo" apply --reverse --check "$patch" >/dev/null 2>&1; then
      log "already applied: ${name}/${base}"
    elif git -C "$repo" apply --check "$patch" >/dev/null 2>&1; then
      git -C "$repo" apply "$patch"
      log "applied: ${name}/${base}"
    else
      die "cannot apply ${name}/${base} to ${repo}"
    fi
  done
}

apply_series llvm "$LLVM_SRC"
apply_series swift "${TC_SRC}/swift"
