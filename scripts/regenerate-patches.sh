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
# The swift series has more than one patch (0002 and on are separate so each can
# go upstream on its own), and a snapshot of the tree would fold them all into
# 0001. Regenerate it by hand: apply 0001, commit, apply the rest, diff.
shopt -s nullglob
swift_patches=("${TOOLCHAIN_ROOT}/patches/swift"/*.patch)
if (( ${#swift_patches[@]} > 1 )); then
  warn "not snapshotting swift: it has ${#swift_patches[@]} patches; regenerate them by hand"
else
  snapshot swift "${TC_SRC}/swift" 0001-wasi-host-support.patch
fi
