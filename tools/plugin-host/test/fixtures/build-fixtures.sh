#!/usr/bin/env bash
# Rebuild the test fixtures. Needs wasi-sdk (WASI_SDK_PATH, or the one
# scripts/00-fetch-sources.sh fetches).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${here}/../../../../env.sh"
"${WASI_SDK_PATH}/bin/clang" --target=wasm32-wasip1 -Os -Wall -Wextra -Werror \
  -o "${here}/echo-plugin.wasm" "${here}/echo-plugin.c"
"${WASI_SDK_PATH}/bin/llvm-strip" "${here}/echo-plugin.wasm"
ls -l "${here}/echo-plugin.wasm"
