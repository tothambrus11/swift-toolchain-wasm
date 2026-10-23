// LSP on top of sourcekitd, one message at a time.
//
// The server never reads or writes a stream itself. It is handed one JSON-RPC
// message at a time (handle) and hands back what it has to say through a
// callback, so the same code serves stdio (main_stdio.cpp) and a host that
// calls into the module per message (reactor.cpp, for a browser).
//
// Work nobody is waiting for, publishing diagnostics after an edit, is not
// done while handling the edit. It is queued and done by idle(), which the
// host calls when no message is waiting. On one thread that is what keeps
// typing responsive: an edit costs a text replacement, not a type check.

#pragma once

#include "json.h"
#include "options.h"
#include "skd.h"
#include "text.h"
#include "translate.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lite {

class Server {
public:
  /// Receives each outgoing message as compact JSON, without framing.
  using Sink = std::function<void(std::string)>;

  Server(Options Opts, Sink Send);
  ~Server();
  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;
  Server(Server &&) = delete;
  Server &operator=(Server &&) = delete;

  /// Handles one incoming JSON-RPC message (no framing). Replies, and the
  /// notifications it causes, reach the sink before this returns.
  void handle(std::string_view Message);

  /// Whether idle() has anything to do.
  [[nodiscard]] bool hasIdleWork() const noexcept { return !DiagnosticsQueue.empty(); }
  /// Does one unit of deferred work (diagnosing one open document) and
  /// returns whether more remains.
  bool idle();

  /// Whether "exit" was received: the host stops, and returns exitCode().
  [[nodiscard]] bool exited() const noexcept { return Exited; }
  [[nodiscard]] int exitCode() const noexcept { return ShutdownRequested ? 0 : 1; }

private:
  struct Document {
    std::string URI;
    std::string Path;
    std::string Text;
    std::int64_t Version = 0;
    LineTable Lines;
    std::string ShadowPath;

    [[nodiscard]] DocumentView view(Encoding In) const noexcept { return {Text, Lines, In}; }
  };

  /// Open documents first, then the disk.
  class Files final : public FileSource {
  public:
    explicit Files(const std::map<std::string, Document, std::less<>> &Open) noexcept
        : Docs(&Open) {}
    [[nodiscard]] std::optional<std::string> text(const std::string &Path) override;
    [[nodiscard]] std::string uri(const std::string &Path) override;

  private:
    const std::map<std::string, Document, std::less<>> *Docs;
  };

  skd::Session SourceKit;
  Options Opts;
  Sink Send;
  Encoding Enc = Encoding::UTF16;
  bool SnippetSupport = false;
  bool Initialized = false;
  bool ShutdownRequested = false;
  bool Exited = false;
  std::map<std::string, Document, std::less<>> Docs; // by path
  Files Sources{Docs};
  std::deque<std::string> DiagnosticsQueue; // paths, next first
  std::vector<std::string> ProjectFiles;    // absolute, sorted
  bool ProjectFilesStale = true;
  std::string ModuleName;
  std::string ShadowDir;
  bool OwnsShadowDir = false;
  std::string OverlayPath;
  bool OverlayStale = true;
  unsigned ShadowCounter = 0;

  // Lifecycle and documents.
  [[nodiscard]] json::Value initialize(const json::Value &Params);
  void didOpen(const json::Value &Params);
  void didChange(const json::Value &Params);
  void didClose(const json::Value &Params);
  [[nodiscard]] Document *document(const json::Value &Params);

  // Requests.
  [[nodiscard]] json::Value completion(const json::Value &Params);
  [[nodiscard]] json::Value hover(const json::Value &Params);
  [[nodiscard]] json::Value definition(const json::Value &Params);
  [[nodiscard]] json::Value references(const json::Value &Params);
  [[nodiscard]] json::Value documentHighlight(const json::Value &Params);
  [[nodiscard]] json::Value documentSymbol(const json::Value &Params);
  [[nodiscard]] json::Value signatureHelp(const json::Value &Params);
  void dispatchRequest(const json::Value &Id, const std::string &Method, const json::Value &Params);

  // Diagnostics.
  void queueDiagnostics(const std::string &FirstPath);
  void publishDiagnostics(const Document &Doc);

  // The project and its compiler arguments.
  void refreshProject();
  [[nodiscard]] std::vector<std::string> compilerArgs();
  void setUpShadowDirectory();
  void writeShadow(Document &Doc);
  void writeOverlay();

  /// Sends a sourcekitd request, logging failures and latency.
  [[nodiscard]] skd::Reply send(const json::Value &Request);

  void reply(const json::Value &Id, json::Value Result);
  void replyError(const json::Value &Id, int Code, std::string_view Message);
  void notify(std::string_view Method, json::Value Params);
  void trace(std::string_view Line) const;
};

/// Runs queued sourcekitd work nobody waits for. On WASI that is the
/// cooperative queues' pending work; natively it is a no-op, libdispatch has
/// threads for it. Defined in pump.cpp.
unsigned runPendingSourceKitWork() noexcept;

} // namespace lite
