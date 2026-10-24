// The Swift compiler, as an export of the same module as the language server.
//
// Stage 36 links swift-frontend-lsp.wasm: the reactor's lsp_* exports
// (reactor.cpp) and this one, over one copy of the compiler's libraries. A
// browser then downloads and compiles one ~150 MB module instead of
// swift-frontend.wasm and sourcekit-lsp-lite-reactor.wasm, which share most of
// their code.
//
//   status = swift_frontend_main(argc, argv)
//
// does what `swift-frontend` does with the same arguments: argv[0] is the
// program name ("swift-frontend"), argv[1] usually "-frontend". argv and its
// strings are in linear memory (lsp_alloc), and stay owned by the host.
//
// One call per instance. swift::mainEntry runs the compiler as a process
// would: it registers LLVM's options in globals, and its InitLLVM shuts LLVM
// down on return (llvm_shutdown), so neither a second compile nor the
// lsp_* exports may use that instance afterwards. The host instantiates the
// compiled WebAssembly.Module again for each compile, which is cheap: the
// expensive part, compiling the module, is shared. A compiler error that
// ends in exit() (llvm::report_fatal_error) reaches the host as WASI
// proc_exit, which the host treats as that status.
//
// Linked only on wasm: natively this would need the compiler's own
// libraries (libswiftDriverTool and its dependencies), which a toolchain does
// not ship, so CMakeLists.txt does not build it.

#include <cstdio>

#include "llvm/Support/raw_ostream.h"

// swift/DriverTool/DriverTool.h, declared here rather than included: that
// header pulls in the Swift include tree for one declaration. The signature
// is checked where it matters, at the link (C++ mangling includes it).
namespace swift {
int mainEntry(int argc_, const char **argv_); // NOLINT(readability-identifier-naming): upstream's
} // namespace swift

#if defined(__wasm__)
#define LSP_EXPORT(Name) __attribute__((export_name(Name)))
#else
#define LSP_EXPORT(Name)
#endif

extern "C" LSP_EXPORT("swift_frontend_main") int swift_frontend_main(int Argc, const char **Argv) {
  const int Status = swift::mainEntry(Argc, Argv);
  // A command's exit would flush these; a reactor's export just returns.
  llvm::outs().flush();
  llvm::errs().flush();
  (void)std::fflush(nullptr);
  return Status;
}
