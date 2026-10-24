#!/usr/bin/env bash
# Source this: fetch_wasm_sdk downloads the Swift SDK for WebAssembly that env.sh
# pins, checks its checksum, unpacks it under the cached sources, and sets
#
#   WASM_SDK_ROOT    the directory holding the .artifactbundle (a --swift-sdks-path)
#   WASM_SDK_ID      the bundle's artifact id, for `swift build --swift-sdk`
#   WASM_SDK_BUNDLE  the wasm32-unknown-wasip1 directory: WASI.sdk, swift.xctoolchain
#
# Stage 40 packs the sysroot from it; with macros on, Stage 30 links the
# compiler's Swift code against its runtime and Stage 37 builds plugins with it.
#
# It is downloaded rather than taken from `swift sdk install`, whose install
# location three CI runs failed to find although `swift sdk list` showed it.

fetch_wasm_sdk() {
  WASM_SDK_ROOT="${TC_SRC}/swift-wasm-sdk"
  if [[ ! -d "$WASM_SDK_ROOT" ]]; then
    log "downloading the wasm SDK bundle (${SWIFT_TAG})"
    local tmp actual
    tmp="$(mktemp -d)"
    curl -fsSL --retry 3 -o "${tmp}/wasm-sdk.tar.gz" "$SWIFT_WASM_SDK_URL"
    actual="$(sha256sum "${tmp}/wasm-sdk.tar.gz" | cut -d' ' -f1)"
    if [[ "$actual" != "$SWIFT_WASM_SDK_CHECKSUM" ]]; then
      rm -rf "$tmp"
      die "wasm SDK checksum mismatch: expected ${SWIFT_WASM_SDK_CHECKSUM}, got ${actual}"
    fi
    mkdir -p "$WASM_SDK_ROOT"
    tar -xzf "${tmp}/wasm-sdk.tar.gz" -C "$WASM_SDK_ROOT"
    rm -rf "$tmp"
  fi
  WASM_SDK_ID="${SWIFT_TAG}_wasm"
  WASM_SDK_BUNDLE="$(dirname "$(find -L "$WASM_SDK_ROOT" -maxdepth 5 -type d -name WASI.sdk \
    -path '*wasm32-unknown-wasip1/*' -print -quit)")"
  if [[ ! -d "${WASM_SDK_BUNDLE}/WASI.sdk" ]]; then
    warn "no WASI.sdk under ${WASM_SDK_ROOT}; its contents are:"
    find -L "$WASM_SDK_ROOT" -maxdepth 3 >&2 2>/dev/null || true
    die "wasm SDK bundle did not contain a WASI.sdk directory"
  fi
  # Until macro support is the default, the default build ships the SDK as it is.
  if [[ "${SWIFT_MACROS:-0}" == "1" ]]; then
    install_wasi_libc_modulemap
  fi
}

# The SDK's wasi-libc.modulemap predates the fix in patches/swift (C++
# interoperability on WASI), and the SDK is prebuilt, so put the patched one from
# the Swift sources in its place. Nothing else in the SDK changes: the stdlib's
# modules reference the libc module by name, not by its layout.
install_wasi_libc_modulemap() {
  local fixed="${SWIFT_SRC:-${TC_SRC}/swift}/stdlib/public/Platform/wasi-libc.modulemap"
  [[ -f "$fixed" ]] || return 0
  local target
  for target in "${WASM_SDK_BUNDLE}/swift.xctoolchain/usr/lib/swift_static/wasi/wasm32/wasi-libc.modulemap" \
                "${WASM_SDK_BUNDLE}/swift.xctoolchain/usr/lib/swift/wasi/wasm32/wasi-libc.modulemap"; do
    [[ -f "$target" ]] && ! cmp -s "$fixed" "$target" && cp "$fixed" "$target"
  done
  return 0
}

# The sysroot the compiler's own Swift code (ASTGen, MacroEvaluation, the
# compiler's swift-syntax) is compiled against when Stage 30 builds with macros:
# the Swift SDK's WASI.sdk, whose libc++ 21 headers the Swift 6.3.3 clang inside
# swiftc parses cleanly, with one change, written to $1:
#
#   _LIBCPP_HAS_THREADS 1 (and the pthread thread API) in __config_site.
#
# Why not either sysroot as it is:
#
#   * wasi-sdk 34's own sysroot is what the C++ of the compiler is built
#     against, so it would be the obvious choice. Its libc++ is version 23,
#     which requires Clang 21.1 ("Libc++ only supports Clang 21 and later";
#     swiftc's clang is 21.0), and its textual <wchar.h> is redefined when
#     clang builds the `std` module for C++ interoperability. It also uses a
#     multiarch eh/noeh layout that clang 21 does not search.
#   * The Swift SDK's WASI.sdk is built with _LIBCPP_HAS_THREADS 0, so the
#     LLVM headers ASTGen imports fail: llvm/Support/Mutex.h uses
#     std::recursive_mutex unconditionally. The single-threaded wasi-libc in
#     the SDK does declare the pthread API, so turning threads on in the
#     headers is all that is missing.
#
# Mixing libc++ versions this way is sound because nothing crosses between them
# that could differ: both are ABI version 2 (std::__2), whose layouts libc++
# keeps stable, and every inline libc++ function carries an ABI tag naming its
# version (_LIBCPP_HIDE_FROM_ABI), so the two copies never merge at link time.
# The non-inline symbols the Swift side references resolve to wasi-sdk's libc++,
# the one the rest of the compiler links.
make_swift_host_sysroot() {
  local dest="$1" sdk="${WASM_SDK_BUNDLE}/WASI.sdk" entry
  rm -rf "$dest"
  mkdir -p "${dest}/include"
  for entry in "$sdk"/*; do
    [[ "$(basename "$entry")" == include ]] || ln -s "$entry" "${dest}/"
  done
  for entry in "${sdk}/include"/*; do
    [[ "$(basename "$entry")" == c++ ]] || ln -s "$entry" "${dest}/include/"
  done
  # A tree of symlinks, so libc++'s module map resolves its headers next to it.
  cp -as "${sdk}/include/c++" "${dest}/include/c++"
  local config="${dest}/include/c++/v1/__config_site"
  rm "$config"
  sed -e 's/^#define _LIBCPP_HAS_THREADS 0$/#define _LIBCPP_HAS_THREADS 1/' \
      -e 's/^#define _LIBCPP_HAS_THREAD_API_PTHREAD 0$/#define _LIBCPP_HAS_THREAD_API_PTHREAD 1/' \
      "${sdk}/include/c++/v1/__config_site" > "$config"
  grep -q '^#define _LIBCPP_HAS_THREADS 1$' "$config" &&
    grep -q '^#define _LIBCPP_HAS_THREAD_API_PTHREAD 1$' "$config" ||
    die "the Swift SDK's libc++ __config_site changed; make_swift_host_sysroot needs updating"
}
