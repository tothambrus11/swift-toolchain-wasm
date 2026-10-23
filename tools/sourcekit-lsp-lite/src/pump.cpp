// Running sourcekitd's queued work on a host without threads.
//
// On WASI, sourcekitd's work queues are cooperative
// (Concurrency-cooperative.cpp in this repository's swift patch): work runs
// when somebody waits for it. A synchronous request waits, so requests are
// answered; but work nobody waits for, such as releasing an AST or posting a
// notification, runs only when SourceKit::WorkQueue::runPendingWork() is
// called. The server calls this after every message and every idle step.
//
// sourcekitd's C API has no entry point for this. Rather than add one to the
// patch series, this file calls the C++ function the patch declares, which
// is why the wasm build compiles it against SourceKit's and LLVM's headers.

#include "server.h"

#if defined(__wasi__)
#include "SourceKit/Support/Concurrency.h"
#endif

namespace lite {

unsigned runPendingSourceKitWork() noexcept {
#if defined(__wasi__)
  return SourceKit::WorkQueue::runPendingWork();
#else
  return 0; // libdispatch runs it on its own threads
#endif
}

} // namespace lite
