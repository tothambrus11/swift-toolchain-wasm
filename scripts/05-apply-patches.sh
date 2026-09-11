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

  # A cached source tree already carries the previous run's patches. Reapplying them
  # rewrites those files and moves their mtimes, and one of them is Identifier.h, which
  # is included across the compiler — so ninja then rebuilds almost everything to
  # reproduce a tree it already had. That turned every retry into a four-hour build.
  # Check first, and touch nothing when the series is already exactly what is applied.
  local patch base all_applied=1
  for patch in "${patches[@]}"; do
    git -C "$repo" apply --reverse --check "$patch" >/dev/null 2>&1 || { all_applied=0; break; }
  done
  if (( all_applied )); then
    log "${name}: series already applied, leaving the tree untouched"
    return 0
  fi

  # The series is not what is in the tree — either nothing is applied, or a cached tree
  # carries an older series. Reset so a changed series applies to a clean checkout
  # instead of on top of the previous one.
  if ! git -C "$repo" diff --quiet 2>/dev/null; then
    log "resetting ${name} checkout: the series differs from what is applied"
    git -C "$repo" checkout -- . 2>/dev/null || true
    git -C "$repo" clean -fdq -- "*.inc" 2>/dev/null || true
  fi

  for patch in "${patches[@]}"; do
    base="$(basename "$patch")"
    if git -C "$repo" apply --check "$patch" >/dev/null 2>&1; then
      git -C "$repo" apply "$patch"
      log "applied: ${name}/${base}"
    else
      die "cannot apply ${name}/${base} to ${repo}"
    fi
  done
}

apply_series llvm "$LLVM_SRC"
apply_series swift "${TC_SRC}/swift"
