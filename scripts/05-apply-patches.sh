#!/usr/bin/env bash
# Stage 05 — apply Yukibana's patch series to the checked-out sources.
#
# Two repositories need patches, for two different reasons:
#
#   llvm/  — POSIX facilities LLVM assumes that wasm32-wasip1 simply lacks: processes,
#            signals, sockets, resource limits, setjmp. All guarded on __wasi__ so a
#            native build of the same tree is unaffected.
#   swift/ — build-system assumptions that break when the host stdlib comes prebuilt
#            from an SDK rather than being built in-tree, and, for SourceKit, the
#            libdispatch, dladdr and dynamic linking a WASI host does not have.
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
  #
  # Resetting and reapplying rewrites every patched file, though, and the same
  # Identifier.h problem as above would turn adding one hunk anywhere in the series into
  # a full compiler rebuild. So remember what each touched file held and when it was
  # last modified, and afterwards give every file that came out byte-identical its old
  # mtime back. Ninja then rebuilds what the new series actually changed, and nothing
  # else. Files the series creates are untracked, so they are recorded and removed too.
  local before
  before="$(mktemp)"
  {
    git -C "$repo" diff --name-only -z 2>/dev/null
    git -C "$repo" ls-files --others --exclude-standard -z 2>/dev/null
  } | while IFS= read -r -d '' file; do
    [[ -f "${repo}/${file}" ]] || continue
    printf '%s\t%s\t%s\n' "$(sha1sum < "${repo}/${file}" | cut -d' ' -f1)" \
      "$(stat -c '%.9Y' "${repo}/${file}")" "$file"
  done > "$before"

  if [[ -s "$before" ]]; then
    log "resetting ${name} checkout: the series differs from what is applied"
    git -C "$repo" checkout -- . 2>/dev/null || true
    git -C "$repo" ls-files --others --exclude-standard -z |
      (cd "$repo" && xargs -0 -r rm -f --)
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

  local hash mtime file kept=0 changed=0
  while IFS=$'\t' read -r hash mtime file; do
    if [[ -f "${repo}/${file}" &&
          "$(sha1sum < "${repo}/${file}" | cut -d' ' -f1)" == "$hash" ]]; then
      touch -m -d "@${mtime}" "${repo}/${file}"
      kept=$((kept + 1))
    else
      changed=$((changed + 1))
    fi
  done < "$before"
  rm -f "$before"
  (( kept + changed == 0 )) ||
    log "${name}: ${kept} files unchanged by the new series kept their mtimes; ${changed} changed"
}

apply_series llvm "$LLVM_SRC"
apply_series swift "${TC_SRC}/swift"
