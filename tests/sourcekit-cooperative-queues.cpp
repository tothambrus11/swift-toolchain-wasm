// Tests SourceKit's work queues as the swift patch series implements them for WASI
// (tools/SourceKit/lib/Support/Concurrency-cooperative.cpp): one thread, no
// libdispatch, work that runs when somebody waits for it.
//
// Built and run by scripts/35-sourcekitd-wasm.sh, linked against the cross-built
// libSourceKitSupport.a. Exits 0 when every check passes. With an argument, it then
// waits on a semaphore nothing can signal, which must be reported (and abort) rather
// than hang: libdispatch would deadlock there too, and a single-threaded host must say
// so instead of spinning forever.

#include "SourceKit/Support/Concurrency.h"

#include <cstdio>
#include <string>

using namespace SourceKit;

static std::string Log;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::printf("FAIL line %d: %s (log: \"%s\")\n", __LINE__, #condition,   \
                  Log.c_str());                                                \
      return 1;                                                                \
    }                                                                          \
  } while (0)

int main(int argc, char **argv) {
  (void)argv;

  // The shape of sourcekitd_send_request_sync: the request is dispatched, its
  // handler dispatches more work, and the caller waits on a semaphore that the
  // last piece signals. Nothing runs at submission; the wait runs it all.
  {
    Log.clear();
    WorkQueue Q(WorkQueue::Dequeuing::Serial, "request");
    Semaphore Sema(0);
    WorkQueue::dispatchConcurrent([&] {
      Log += "a";
      Q.dispatch([&] {
        Log += "b";
        Sema.signal();
      });
    });
    CHECK(Log.empty());
    CHECK(!Sema.wait());
    CHECK(Log == "ab");
  }

  // dispatchSync on a serial queue runs after what the queue already holds.
  {
    Log.clear();
    WorkQueue Q(WorkQueue::Dequeuing::Serial, "serial");
    Q.dispatch([&] { Log += "1"; });
    Q.dispatch([&] { Log += "2"; });
    Q.dispatchSync([&] { Log += "S"; });
    CHECK(Log == "12S");
  }

  // A barrier on a concurrent queue waits for the items before it, and the
  // items after it wait for the barrier.
  {
    Log.clear();
    WorkQueue C(WorkQueue::Dequeuing::Concurrent, "concurrent");
    Semaphore Done(0);
    C.dispatch([&] { Log += "r1"; });
    C.dispatchBarrier([&] { Log += "B"; });
    C.dispatch([&] {
      Log += "r2";
      Done.signal();
    });
    CHECK(!Done.wait());
    CHECK(Log == "r1Br2");
  }

  // A wait inside a serial queue's item may run other queues' work, but never
  // the next item of its own queue: serial means one at a time, on the stack
  // as much as on threads.
  {
    Log.clear();
    WorkQueue Q(WorkQueue::Dequeuing::Serial, "outer");
    WorkQueue Other(WorkQueue::Dequeuing::Serial, "other");
    Semaphore Inner(0), Outer(0);
    Q.dispatch([&] {
      Log += "x";
      Q.dispatch([&] {
        Log += "y";
        Outer.signal();
      });
      Other.dispatch([&] {
        Log += "o";
        Inner.signal();
      });
      Inner.wait();
      Log += "X";
    });
    CHECK(!Outer.wait());
    CHECK(Log == "xoXy");
  }

  // A suspended queue holds its work until resumed.
  {
    Log.clear();
    WorkQueue Q(WorkQueue::Dequeuing::Serial, "suspended");
    Q.suspend();
    Q.dispatch([&] { Log += "s"; });
    CHECK(WorkQueue::runPendingWork() == 0);
    Q.resume();
    CHECK(WorkQueue::runPendingWork() == 1);
    CHECK(Log == "s");
  }

  // A timed wait that nothing can satisfy times out at once: there is no
  // other thread that could signal it later.
  {
    Semaphore S(0);
    CHECK(S.wait(1000));
  }

  // Work keeps its queue alive after the last handle to it is gone.
  {
    Log.clear();
    {
      WorkQueue Q(WorkQueue::Dequeuing::Serial, "released");
      Q.dispatch([&] { Log += "late"; });
    }
    CHECK(WorkQueue::runPendingWork() == 1);
    CHECK(Log == "late");
  }

  std::printf("cooperative work queues: all checks passed\n");
  std::fflush(stdout);

  if (argc > 1) {
    Semaphore Never(0);
    Never.wait(); // Must abort with "would block forever".
    std::printf("FAIL: an unsatisfiable wait returned\n");
    return 1;
  }
  return 0;
}
