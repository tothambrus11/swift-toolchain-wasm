#!/usr/bin/env bash
# Stage 36 — sourcekit-lsp-lite for a wasm host.
#
# sourcekit-lsp-lite (tools/sourcekit-lsp-lite) is a thin LSP server over
# sourcekitd's C API, in C++, so it links straight into the same module as
# Stage 35's static, single-threaded sourcekitd. This stage compiles its sources
# with the WASI SDK, with the warnings its native build uses as errors, and
# links them the way Stage 35 links sourcekitd-request: same libraries, same
# flags, same stack, with the server's objects in place of the driver's.
#
#   out/sourcekit-lsp-lite.wasm          a WASI command: LSP on stdin/stdout,
#                                        for wasmtime and tests
#   out/sourcekit-lsp-lite-reactor.wasm  a WASI reactor exporting lsp_handle,
#                                        lsp_idle, ... (src/reactor.cpp), for a
#                                        browser worker: a JS host calls it per
#                                        message, so nothing blocks on stdin
#
# The link command is not written out here. It is the one CMake generated for
# sourcekitd-request, read back with `ninja -t commands`, because that is the
# only list of sourcekitd's libraries guaranteed to match what Stage 35 built.
#
# LITE_COMPILE_ONLY=1 compiles the server's own sources and stops, which needs
# only the WASI SDK and sourcekitd.h (SOURCEKITD_INCLUDE), not Stage 35.
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

: "${SWIFT_SRC:=${TC_SRC}/swift}"
: "${SOURCEKITD_INCLUDE:=${SWIFT_SRC}/tools/SourceKit/tools/sourcekitd/include/sourcekitd}"
: "${LITE_COMPILE_ONLY:=0}"

LITE_SRC="${TOOLCHAIN_ROOT}/tools/sourcekit-lsp-lite"
LLVM_BUILD="${TC_BUILD}/wasm-llvm"
SWIFT_BUILD="${TC_BUILD}/wasm-swift"
OBJ="${TC_BUILD}/sourcekit-lsp-lite-wasm"

[[ -x "${WASI_SDK_PATH}/bin/clang++" ]] || die "no WASI SDK at ${WASI_SDK_PATH}; run 00-fetch-sources.sh"
[[ -f "${SOURCEKITD_INCLUDE}/sourcekitd.h" ]] || die "no sourcekitd.h in ${SOURCEKITD_INCLUDE}"

mapfile -t WARNINGS < <(grep '^-' "${LITE_SRC}/cmake/warnings.txt")
CXXFLAGS=(
  --target="${WASM_TARGET}" -std=c++20 -O2
  -fno-exceptions -fno-rtti
  -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL
  -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID
  "${WARNINGS[@]}" -Werror
  -isystem "${SOURCEKITD_INCLUDE}"
)
# pump.cpp calls SourceKit::WorkQueue::runPendingWork() (the swift patch adds
# it), so it needs SourceKit's and LLVM's headers; they are not ours to warn
# about, hence -isystem.
PUMP_FLAGS=(
  -isystem "${SWIFT_SRC}/tools/SourceKit/include"
  -isystem "${LLVM_SRC}/llvm/include" -isystem "${LLVM_BUILD}/include"
)

SOURCES=(json text framing options requests translate skd server main_stdio reactor)
mkdir -p "$OBJ"
log "compiling sourcekit-lsp-lite for ${WASM_TARGET}"
for name in "${SOURCES[@]}"; do
  "${WASI_SDK_PATH}/bin/clang++" "${CXXFLAGS[@]}" -c "${LITE_SRC}/src/${name}.cpp" -o "${OBJ}/${name}.o"
done
if [[ -f "${LLVM_BUILD}/include/llvm/Config/llvm-config.h" ]]; then
  "${WASI_SDK_PATH}/bin/clang++" "${CXXFLAGS[@]}" "${PUMP_FLAGS[@]}" \
    -c "${LITE_SRC}/src/pump.cpp" -o "${OBJ}/pump.o"
elif [[ "$LITE_COMPILE_ONLY" == 1 ]]; then
  warn "no LLVM build tree (${LLVM_BUILD}); not compiling pump.cpp"
else
  die "no LLVM build tree at ${LLVM_BUILD}; run 20-llvm-wasm.sh"
fi
if [[ "$LITE_COMPILE_ONLY" == 1 ]]; then
  log "compiled ${#SOURCES[@]} sources into ${OBJ}; LITE_COMPILE_ONLY=1, not linking"
  exit 0
fi

[[ -f "${SWIFT_BUILD}/lib/libsourcekitdInProc.a" ]] || die "no static sourcekitdInProc; run 35-sourcekitd-wasm.sh"

# sourcekitd-request's link line, from the build graph Stage 35 used.
link_command="$(ninja -C "$SWIFT_BUILD" -t commands -s bin/sourcekitd-request 2>/dev/null | tail -1)"
[[ -n "$link_command" ]] || link_command="$(ninja -C "$SWIFT_BUILD" -t commands sourcekitd-request | tail -1)"
[[ "$link_command" == *sourcekitd-request.cpp.o* ]] ||
  die "could not find sourcekitd-request's link command in ${SWIFT_BUILD}: ${link_command:0:200}"

# Rewrites that command: the server's objects instead of the driver's, a new
# output, and for the reactor no main and -mexec-model=reactor. Refuses
# anything it does not understand (a response file, a compound command)
# rather than guessing.
relink() {
  local output="$1" mode="$2"
  shift 2
  LINK_COMMAND="$link_command" python3 - "$output" "$mode" "$@" <<'PY'
import os, shlex, subprocess, sys
output, mode, objects = sys.argv[1], sys.argv[2], sys.argv[3:]
command = os.environ["LINK_COMMAND"].strip()
if command.startswith(": && "):
    command = command[len(": && "):]
if command.endswith(" && :"):
    command = command[: -len(" && :")]
if "&&" in command or ";" in command:
    sys.exit(f"error: not a single link command: {command[:300]}")
args = shlex.split(command)
if any(a.startswith("@") for a in args):
    sys.exit("error: the link uses a response file; relink sourcekitd-request with "
             "`ninja -d keeprsp` so it is kept, then run this stage again")
driver = [i for i, a in enumerate(args) if a.endswith("sourcekitd-request.cpp.o")]
if len(driver) != 1 or "-o" not in args:
    sys.exit(f"error: unexpected link command: {command[:300]}")
args[driver[0]:driver[0] + 1] = objects
args[args.index("-o") + 1] = output
if mode == "reactor":
    args.insert(1, "-mexec-model=reactor")
print("+ " + shlex.join(args)[:400] + " ...", file=sys.stderr)
sys.exit(subprocess.call(args))
PY
}

COMMON_OBJECTS=()
for name in json text framing options requests translate skd server pump; do
  COMMON_OBJECTS+=("${OBJ}/${name}.o")
done

log "linking sourcekit-lsp-lite.wasm (a WASI command)"
(cd "$SWIFT_BUILD" && relink "${OBJ}/sourcekit-lsp-lite.wasm" command \
  "${COMMON_OBJECTS[@]}" "${OBJ}/main_stdio.o") || die "linking the command failed"
log "linking sourcekit-lsp-lite-reactor.wasm (a WASI reactor)"
(cd "$SWIFT_BUILD" && relink "${OBJ}/sourcekit-lsp-lite-reactor.wasm" reactor \
  "${COMMON_OBJECTS[@]}" "${OBJ}/reactor.o") || die "linking the reactor failed"

install -D "${OBJ}/sourcekit-lsp-lite.wasm" "${TC_OUT}/sourcekit-lsp-lite.wasm"
install -D "${OBJ}/sourcekit-lsp-lite-reactor.wasm" "${TC_OUT}/sourcekit-lsp-lite-reactor.wasm"
log "wrote ${TC_OUT}/sourcekit-lsp-lite.wasm ($(du -h "${TC_OUT}/sourcekit-lsp-lite.wasm" | cut -f1))"
log "wrote ${TC_OUT}/sourcekit-lsp-lite-reactor.wasm ($(du -h "${TC_OUT}/sourcekit-lsp-lite-reactor.wasm" | cut -f1))"

# The reactor must export what a host calls; a missing export would only show
# up in the browser otherwise. Read from the module's export section.
python3 - "${TC_OUT}/sourcekit-lsp-lite-reactor.wasm" <<'PY' || die "the reactor is missing exports"
import sys
data = open(sys.argv[1], "rb").read()
def leb(i):
    result = shift = 0
    while True:
        b = data[i]; i += 1
        result |= (b & 0x7F) << shift; shift += 7
        if b < 0x80:
            return result, i
i, exports = 8, set()
while i < len(data):
    section, i = data[i], i + 1
    size, i = leb(i)
    if section == 7:
        count, j = leb(i)
        for _ in range(count):
            n, j = leb(j)
            exports.add(data[j:j + n].decode()); j += n + 1
            _, j = leb(j)
    i += size
wanted = {"_initialize", "memory", "lsp_alloc", "lsp_free", "lsp_handle", "lsp_idle",
          "lsp_has_idle_work", "lsp_output", "lsp_exit_code"}
missing = wanted - exports
if missing:
    sys.exit(f"missing exports: {sorted(missing)}")
print("reactor exports: " + ", ".join(sorted(wanted)), file=sys.stderr)
PY
