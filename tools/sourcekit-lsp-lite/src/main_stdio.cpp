// sourcekit-lsp-lite over stdin/stdout, with Content-Length framing.
//
// Natively, deferred work (diagnostics) runs once stdin has been quiet for a
// moment, so a burst of keystrokes costs one type check, not one per key. On
// WASI there is no dependable way to ask whether stdin has input, so deferred
// work runs as soon as no complete message is buffered; a browser host uses
// the reactor entry points (reactor.cpp) and decides itself when to idle.

#include "framing.h"
#include "options.h"
#include "server.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

#if !defined(__wasi__)
#include <poll.h>
#endif

namespace {

/// How long stdin must be quiet before deferred work runs, natively.
constexpr int QuietMilliseconds = 50;

bool writeAll(int FD, std::string_view Bytes) {
  while (!Bytes.empty()) {
    const ssize_t N = ::write(FD, Bytes.data(), Bytes.size());
    if (N < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    Bytes.remove_prefix(static_cast<std::size_t>(N));
  }
  return true;
}

/// Whether input arrives on stdin within \p Milliseconds.
bool inputWithin([[maybe_unused]] int Milliseconds) {
#if defined(__wasi__)
  return false;
#else
  pollfd In{STDIN_FILENO, POLLIN, 0};
  return ::poll(&In, 1, Milliseconds) != 0; // an error: let read() report it
#endif
}

} // namespace

int main(int argc, char **argv) {
  const std::span<char *> Argv(argv, static_cast<std::size_t>(argc));
  const std::vector<std::string> Args(Argv.begin() + 1, Argv.end());
  lite::Options Opts;
  for (const std::string &A : Args)
    if (A == "--help" || A == "-h") {
      (void)std::fputs(lite::usage().data(), stdout);
      return 0;
    }
  const std::string Error = lite::parseArguments(Args, Opts);
  if (!Error.empty()) {
    (void)std::fprintf(stderr, "sourcekit-lsp-lite: %s\n\n%s", Error.c_str(), lite::usage().data());
    return 2;
  }

  bool Broken = false;
  lite::Server Server(Opts, [&Broken](const std::string &Message) {
    Broken = Broken || !writeAll(STDOUT_FILENO, lite::frame(Message));
  });
  lite::FrameDecoder Decoder;
  std::array<char, 65536> Buffer{};
  while (!Server.exited() && !Broken) {
    if (std::optional<std::string> Message = Decoder.next()) {
      Server.handle(*Message);
      continue;
    }
    // cppcheck-suppress knownConditionTrueFalse ; true only in the WASI configuration
    if (Server.hasIdleWork() && !inputWithin(QuietMilliseconds)) {
      (void)Server.idle();
      continue;
    }
    const ssize_t N = ::read(STDIN_FILENO, Buffer.data(), Buffer.size());
    if (N < 0 && errno == EINTR)
      continue;
    if (N <= 0)
      break; // the client went away without "exit"
    Decoder.feed(std::string_view(Buffer.data(), static_cast<std::size_t>(N)));
  }
  return Server.exited() ? Server.exitCode() : 1;
}
