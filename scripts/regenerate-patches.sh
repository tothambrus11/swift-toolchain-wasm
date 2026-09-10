#!/usr/bin/env bash
# Development helper: snapshot the working trees back into the patch series, so fixes
# made while debugging a build become reproducible inputs.
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

snapshot() {
  local name="$1" repo="$2" file="$3"
  [[ -d "${repo}/.git" ]] || { warn "skipping ${name}: not a checkout"; return 0; }
  local out="${TOOLCHAIN_ROOT}/patches/${name}/${file}"
  mkdir -p "$(dirname "$out")"
  # `git diff` alone silently omits files that are new to the tree — which is exactly
  # what the WASI implementations of Program.inc and Signals.inc are. Mark untracked
  # files intent-to-add first so they appear in the diff; a build from a clean checkout
  # fails without them, while the machine that wrote them keeps working.
  git -C "$repo" add -AN . >/dev/null 2>&1 || true
  git -C "$repo" diff > "$out"
  if [[ -s "$out" ]]; then
    log "wrote ${out} ($(grep -c '^--- a/' "$out") files, $(wc -l < "$out") lines)"
  else
    rm -f "$out"
    log "${name}: no local changes"
  fi
}

snapshot llvm "$LLVM_SRC" 0001-wasi-host-support.patch
snapshot swift "${TC_SRC}/swift" 0001-wasi-host-support.patch
