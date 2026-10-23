// sourcekit-lsp-lite as a library a JavaScript host calls per message.
//
// A browser cannot block a WASI module on stdin without a worker, a
// SharedArrayBuffer and cross-origin isolation. Linked as a WASI reactor
// (-mexec-model=reactor), the module instead exports these functions, and the
// host calls them from its worker's message handler:
//
//   _initialize()                 once, after instantiating (runs constructors)
//   p = lsp_alloc(n)              room for an n-byte message in linear memory
//   n = lsp_handle(p, len)        handle one JSON-RPC message (no framing);
//                                 returns the byte length of the output
//   p = lsp_output()              the output: zero or more JSON messages, each
//                                 followed by "\n"; valid until the next call
//   lsp_free(p)
//   n = lsp_idle()                one unit of deferred work (diagnostics);
//                                 output as for lsp_handle
//   lsp_has_idle_work()           1 while lsp_idle has something to do
//   lsp_exit_code()               -1 until "exit", then the exit code
//
// The host calls lsp_idle when no message is waiting, e.g. from a timer reset
// by each incoming message, which debounces diagnostics while typing.
//
// The same functions are plain extern "C" natively, which is how the tests
// drive them.

#include "options.h"
#include "server.h"

#include <cstdlib>
#include <memory>
#include <string>

// The one macro here: the attribute that names a wasm export exists only for
// wasm targets, and other compilers warn about it.
#if defined(__wasm__)
#define LSP_EXPORT(Name) __attribute__((export_name(Name)))
#else
#define LSP_EXPORT(Name)
#endif

namespace {

struct Reactor {
  std::string Output;
  std::unique_ptr<lite::Server> Server;
};

Reactor &reactor() {
  static Reactor R;
  if (!R.Server)
    R.Server = std::make_unique<lite::Server>(lite::Options{}, [](const std::string &Message) {
      reactor().Output.append(Message).push_back('\n');
    });
  return R;
}

} // namespace

extern "C" {

LSP_EXPORT("lsp_alloc") void *lsp_alloc(std::size_t Size) {
  return std::malloc(Size); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory):
                            // the host owns it
}

LSP_EXPORT("lsp_free") void lsp_free(void *Pointer) {
  std::free(Pointer); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
}

LSP_EXPORT("lsp_handle") std::size_t lsp_handle(const char *Message, std::size_t Length) {
  Reactor &R = reactor();
  R.Output.clear();
  R.Server->handle(std::string_view(Message, Length));
  return R.Output.size();
}

LSP_EXPORT("lsp_idle") std::size_t lsp_idle() {
  Reactor &R = reactor();
  R.Output.clear();
  (void)R.Server->idle();
  return R.Output.size();
}

LSP_EXPORT("lsp_has_idle_work") int lsp_has_idle_work() {
  return reactor().Server->hasIdleWork() ? 1 : 0;
}

LSP_EXPORT("lsp_output") const char *lsp_output() { return reactor().Output.data(); }

LSP_EXPORT("lsp_exit_code") int lsp_exit_code() {
  const lite::Server &S = *reactor().Server;
  return S.exited() ? S.exitCode() : -1;
}

} // extern "C"
