// LSP on top of sourcekitd, one message at a time.
//
// Each LSP feature is one or two sourcekitd requests, chosen the way
// SourceKit-LSP chooses them, minus everything that needs an index, a build
// system or a second thread:
//
//   didOpen/didChange/didClose  editor.open / editor.replacetext /
//                               editor.close (syntactic only), and a shadow
//                               copy of the text for -vfsoverlay
//   publishDiagnostics          diagnostics, from idle()
//   completion                  codecomplete at the start of the typed word,
//                               filtered and ranked here
//   hover                       cursorinfo: annotated decl + doc comment
//   definition                  cursorinfo's location
//   references                  cursorinfo's USR; then, per project file,
//                               cursorinfo on the textual occurrences of the
//                               name until one has that USR, and
//                               relatedidents from there
//   documentHighlight           relatedidents
//   documentSymbol              editor.open of a scratch copy, substructure
//   signatureHelp               signaturehelp

#include "server.h"

#include "requests.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <memory>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace lite {

namespace {

// JSON-RPC and LSP error codes.
constexpr int ParseError = -32700;
constexpr int InvalidRequest = -32600;
constexpr int MethodNotFound = -32601;
constexpr int ServerNotInitialized = -32002;

constexpr int TextDocumentSyncIncremental = 2;
/// How many occurrences of a name references() resolves in one file before
/// deciding the file does not refer to the symbol.
constexpr std::size_t MaxCandidatesPerFile = 64;
constexpr int MaxScanDepth = 16;

using Clock = std::chrono::steady_clock;

double millisecondsSince(Clock::time_point Start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - Start).count();
}

using File = std::unique_ptr<std::FILE, int (*)(std::FILE *)>;

File openFile(const std::string &Path, const char *Mode) {
  return {std::fopen(Path.c_str(), Mode), &std::fclose};
}

std::optional<std::string> readFile(const std::string &Path) {
  const File F = openFile(Path, "rb");
  if (!F)
    return std::nullopt;
  std::string Out;
  std::array<char, 65536> Buf{};
  std::size_t N = 0;
  while ((N = std::fread(Buf.data(), 1, Buf.size(), F.get())) > 0)
    Out.append(Buf.data(), N);
  return Out;
}

bool writeFile(const std::string &Path, std::string_view Text) {
  File F = openFile(Path, "wb");
  if (!F)
    return false;
  const bool Written = std::fwrite(Text.data(), 1, Text.size(), F.get()) == Text.size();
  return std::fclose(F.release()) == 0 && Written;
}

bool isDirectory(const std::string &Path) {
  struct stat St {};
  return ::stat(Path.c_str(), &St) == 0 && S_ISDIR(St.st_mode);
}

/// Every *.swift file under \p Dir, skipping hidden directories and build
/// output.
void collectSwiftFiles(const std::string &Dir, int Depth, std::set<std::string> &Out) {
  if (Depth > MaxScanDepth)
    return;
  const std::unique_ptr<DIR, int (*)(DIR *)> D(::opendir(Dir.c_str()), &::closedir);
  if (!D)
    return;
  while (const dirent *E = ::readdir(D.get())) { // NOLINT(concurrency-mt-unsafe): one thread
    const std::string_view Name = static_cast<const char *>(E->d_name);
    if (Name.empty() || Name.starts_with('.') || Name == "node_modules" || Name == "build")
      continue;
    std::string Path = Dir + "/" + std::string(Name);
    if (Name.ends_with(".swift"))
      Out.insert(std::move(Path));
    else if (isDirectory(Path))
      collectSwiftFiles(Path, Depth + 1, Out);
  }
}

json::Value stringArray(std::initializer_list<std::string_view> Strings) {
  json::Value A = json::Value::array();
  for (const std::string_view S : Strings)
    A.push(S);
  return A;
}

} // namespace

//===----------------------------------------------------------------------===//
// Plumbing
//===----------------------------------------------------------------------===//

std::optional<std::string> Server::Files::text(const std::string &Path) {
  const auto It = Docs->find(Path);
  if (It != Docs->end())
    return It->second.Text;
  return readFile(Path);
}

std::string Server::Files::uri(const std::string &Path) {
  const auto It = Docs->find(Path);
  return It != Docs->end() ? It->second.URI : uriFromPath(Path);
}

Server::Server(Options O, Sink S) : Opts(std::move(O)), Send(std::move(S)) {}

Server::~Server() {
  for (const auto &[Path, Doc] : Docs)
    if (!Doc.ShadowPath.empty())
      (void)std::remove(Doc.ShadowPath.c_str());
  if (!OverlayPath.empty())
    (void)std::remove(OverlayPath.c_str());
  if (OwnsShadowDir)
    (void)::rmdir(ShadowDir.c_str());
  runPendingSourceKitWork();
}

void Server::trace(std::string_view Line) const {
  if (Opts.Log)
    (void)std::fprintf(stderr, "[sourcekit-lsp-lite] %.*s\n", static_cast<int>(Line.size()),
                       Line.data());
}

skd::Reply Server::send(const json::Value &Request) {
  const auto Start = Clock::now();
  skd::Reply R = SourceKit.send(Request);
  if (Opts.Log) {
    std::array<char, 64> Took{};
    (void)std::snprintf(Took.data(), Took.size(), " %.1f ms", millisecondsSince(Start));
    trace("  " + Request["key.request"].asString() + Took.data() +
          (R.OK ? std::string() : " error: " + R.Error));
  }
  return R;
}

void Server::reply(const json::Value &Id, json::Value Result) {
  json::Value Msg = json::Value::object();
  Msg.set("jsonrpc", "2.0").set("id", Id).set("result", std::move(Result));
  Send(json::serialize(Msg));
}

void Server::replyError(const json::Value &Id, int Code, std::string_view Message) {
  json::Value Err = json::Value::object();
  Err.set("code", Code).set("message", Message);
  json::Value Msg = json::Value::object();
  Msg.set("jsonrpc", "2.0").set("id", Id).set("error", std::move(Err));
  Send(json::serialize(Msg));
}

void Server::notify(std::string_view Method, json::Value Params) {
  json::Value Msg = json::Value::object();
  Msg.set("jsonrpc", "2.0").set("method", Method).set("params", std::move(Params));
  Send(json::serialize(Msg));
}

void Server::handle(std::string_view Message) {
  const auto Start = Clock::now();
  std::string ParseErr;
  const std::optional<json::Value> Parsed = json::parse(Message, &ParseErr);
  if (!Parsed || !Parsed->isObject()) {
    replyError(json::Value(), ParseError, Parsed ? "a message must be a JSON object" : ParseErr);
    return;
  }
  const json::Value &Msg = *Parsed;
  const std::string &Method = Msg["method"].asString();
  const json::Value &Params = Msg["params"];
  if (Method.empty())
    return; // a response to a request of ours; this server sends none

  if (Msg.has("id")) {
    dispatchRequest(Msg["id"], Method, Params);
  } else if (Method == "exit") {
    Exited = true;
  } else if (!Initialized) {
    // Notifications before initialize are dropped, as the spec says.
  } else if (Method == "textDocument/didOpen") {
    didOpen(Params);
  } else if (Method == "textDocument/didChange") {
    didChange(Params);
  } else if (Method == "textDocument/didClose") {
    didClose(Params);
  } else if (Method == "workspace/didChangeWatchedFiles") {
    ProjectFilesStale = true;
  }
  // initialized, didSave, $/cancelRequest, $/setTrace and
  // didChangeConfiguration need nothing: no request is still running when the
  // next message arrives, so there is nothing to cancel.

  runPendingSourceKitWork();
  if (Opts.Log) {
    std::array<char, 64> Took{};
    (void)std::snprintf(Took.data(), Took.size(), " %.1f ms", millisecondsSince(Start));
    trace(Method + Took.data());
  }
}

void Server::dispatchRequest(const json::Value &Id, const std::string &Method,
                             const json::Value &Params) {
  if (!Initialized && Method != "initialize") {
    replyError(Id, ServerNotInitialized, "initialize first");
    return;
  }
  if (ShutdownRequested) {
    replyError(Id, InvalidRequest, "the server is shutting down");
    return;
  }
  if (Method == "initialize") {
    reply(Id, initialize(Params));
    return;
  }
  if (Method == "shutdown") {
    ShutdownRequested = true;
    reply(Id, json::Value());
    return;
  }
  using Handler = json::Value (Server::*)(const json::Value &);
  static constexpr std::array<std::pair<std::string_view, Handler>, 7> Handlers{{
      {"textDocument/completion", &Server::completion},
      {"textDocument/hover", &Server::hover},
      {"textDocument/definition", &Server::definition},
      {"textDocument/references", &Server::references},
      {"textDocument/documentHighlight", &Server::documentHighlight},
      {"textDocument/documentSymbol", &Server::documentSymbol},
      {"textDocument/signatureHelp", &Server::signatureHelp},
  }};
  const auto H = std::find_if(Handlers.begin(), Handlers.end(),
                              [&](const auto &Entry) { return Entry.first == Method; });
  if (H == Handlers.end())
    replyError(Id, MethodNotFound, "unsupported method: " + Method);
  else
    reply(Id, (this->*(H->second))(Params));
}

bool Server::idle() {
  while (!DiagnosticsQueue.empty()) {
    const std::string Path = DiagnosticsQueue.front();
    DiagnosticsQueue.pop_front();
    const auto It = Docs.find(Path);
    if (It == Docs.end())
      continue; // closed since
    publishDiagnostics(It->second);
    runPendingSourceKitWork();
    break;
  }
  return !DiagnosticsQueue.empty();
}

//===----------------------------------------------------------------------===//
// Lifecycle and documents
//===----------------------------------------------------------------------===//

json::Value Server::initialize(const json::Value &Params) {
  Initialized = true;
  applyInitializationOptions(Params["initializationOptions"], Opts);
  if (Opts.Root.empty()) {
    Opts.Root = pathFromURI(Params["rootUri"].asString());
    if (Opts.Root.empty())
      Opts.Root = pathFromURI(Params["workspaceFolders"][std::size_t{0}]["uri"].asString());
    if (Opts.Root.empty())
      Opts.Root = Params["rootPath"].asString();
  }
  while (Opts.Root.size() > 1 && Opts.Root.ends_with('/'))
    Opts.Root.pop_back();
  ProjectFilesStale = true;
  setUpShadowDirectory();

  const json::Value &Caps = Params["capabilities"];
  const auto &Encodings = Caps["general"]["positionEncodings"].elements();
  if (std::any_of(Encodings.begin(), Encodings.end(),
                  [](const json::Value &E) { return E.asString() == "utf-8"; }))
    Enc = Encoding::UTF8;
  SnippetSupport = Caps["textDocument"]["completion"]["completionItem"]["snippetSupport"].asBool();

  json::Value Sync = json::Value::object();
  Sync.set("openClose", true).set("change", TextDocumentSyncIncremental);
  json::Value Completion = json::Value::object();
  Completion.set("triggerCharacters", stringArray({"."})).set("resolveProvider", false);
  json::Value Signature = json::Value::object();
  Signature.set("triggerCharacters", stringArray({"(", ","}));
  json::Value Capabilities = json::Value::object();
  Capabilities.set("positionEncoding", Enc == Encoding::UTF8 ? "utf-8" : "utf-16")
      .set("textDocumentSync", std::move(Sync))
      .set("completionProvider", std::move(Completion))
      .set("hoverProvider", true)
      .set("definitionProvider", true)
      .set("referencesProvider", true)
      .set("documentHighlightProvider", true)
      .set("documentSymbolProvider", true)
      .set("signatureHelpProvider", std::move(Signature));
  json::Value Info = json::Value::object();
  Info.set("name", "sourcekit-lsp-lite").set("version", "0.1.0");
  json::Value Result = json::Value::object();
  Result.set("capabilities", std::move(Capabilities)).set("serverInfo", std::move(Info));
  return Result;
}

void Server::setUpShadowDirectory() {
  if (Opts.ShadowDirectory == "none")
    return;
  std::string Dir = Opts.ShadowDirectory;
  if (Dir.empty()) {
    const char *Tmp = std::getenv("TMPDIR"); // NOLINT(concurrency-mt-unsafe): one thread
    const std::string Base = (Tmp != nullptr && *Tmp != '\0') ? Tmp : "/tmp";
    // A name nobody else is using; getpid() would need wasi-emulated-getpid.
    const auto Ticks = static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
    Dir = Base + "/sourcekit-lsp-lite-" + std::to_string(Ticks);
    OwnsShadowDir = ::mkdir(Dir.c_str(), 0700) == 0;
  } else {
    (void)::mkdir(Dir.c_str(), 0700);
  }
  if (!isDirectory(Dir)) {
    trace("no shadow directory " + Dir + ": completion reads other files as saved");
    OwnsShadowDir = false;
    return;
  }
  ShadowDir = Dir;
  OverlayPath = ShadowDir + "/overlay.yaml";
  OverlayStale = true;
}

void Server::writeShadow(Document &Doc) {
  if (ShadowDir.empty())
    return;
  if (Doc.ShadowPath.empty()) {
    const std::size_t Slash = Doc.Path.rfind('/');
    Doc.ShadowPath = ShadowDir + "/" + std::to_string(++ShadowCounter) + "-" +
                     Doc.Path.substr(Slash == std::string::npos ? 0 : Slash + 1);
    OverlayStale = true;
  }
  if (!writeFile(Doc.ShadowPath, Doc.Text))
    trace("cannot write " + Doc.ShadowPath);
}

void Server::writeOverlay() {
  if (ShadowDir.empty() || !OverlayStale)
    return;
  // An LLVM VFS overlay; JSON is YAML. use-external-names is off so that
  // diagnostics and locations name the project's paths, not the shadows.
  json::Value Roots = json::Value::array();
  for (const auto &[Path, Doc] : Docs) {
    if (Doc.ShadowPath.empty())
      continue;
    json::Value Entry = json::Value::object();
    Entry.set("type", "file").set("name", Path).set("external-contents", Doc.ShadowPath);
    Roots.push(std::move(Entry));
  }
  json::Value Overlay = json::Value::object();
  Overlay.set("version", 0)
      .set("case-sensitive", "true")
      .set("use-external-names", false)
      .set("roots", std::move(Roots));
  if (writeFile(OverlayPath, json::serialize(Overlay)))
    OverlayStale = false;
  else
    trace("cannot write " + OverlayPath);
}

void Server::refreshProject() {
  if (!ProjectFilesStale)
    return;
  ProjectFilesStale = false;
  std::set<std::string> Found;
  if (!Opts.Files.empty()) {
    for (const std::string &F : Opts.Files)
      Found.insert(F.starts_with('/') || Opts.Root.empty() ? F : Opts.Root + "/" + F);
  } else if (Opts.Scan && !Opts.Root.empty()) {
    collectSwiftFiles(Opts.Root, 0, Found);
  }
  // Excluded files stay out unless open: a request about an open file needs
  // that file in the compile.
  const std::string RootSlash = Opts.Root + "/";
  std::erase_if(Found, [&](const std::string &Path) {
    const std::string_view Rel =
        std::string_view(Path).substr(Path.starts_with(RootSlash) ? RootSlash.size() : 0);
    return !Docs.contains(Path) &&
           std::any_of(Opts.Exclude.begin(), Opts.Exclude.end(),
                       [&](const std::string &E) { return Rel.starts_with(E); });
  });
  for (const auto &[Path, Doc] : Docs)
    if (Path.ends_with(".swift"))
      Found.insert(Path);
  ProjectFiles.assign(Found.begin(), Found.end());

  if (!Opts.ModuleName.empty()) {
    ModuleName = Opts.ModuleName;
    return;
  }
  std::vector<std::string> Texts;
  for (const std::string &Path : ProjectFiles)
    if (std::optional<std::string> Text = Sources.text(Path))
      Texts.push_back(std::move(*Text));
  ModuleName = inferModuleName(Texts);
}

std::vector<std::string> Server::compilerArgs() {
  refreshProject();
  std::vector<std::string> Args = ProjectFiles;
  Args.insert(Args.end(), {"-module-name", ModuleName});
  Args.insert(Args.end(), Opts.CompilerArgs.begin(), Opts.CompilerArgs.end());
  writeOverlay();
  if (!ShadowDir.empty() && !OverlayStale)
    Args.insert(Args.end(), {"-vfsoverlay", OverlayPath});
  return Args;
}

Server::Document *Server::document(const json::Value &Params) {
  const auto It = Docs.find(pathFromURI(Params["textDocument"]["uri"].asString()));
  return It == Docs.end() ? nullptr : &It->second;
}

void Server::didOpen(const json::Value &Params) {
  const json::Value &TD = Params["textDocument"];
  const std::string Path = pathFromURI(TD["uri"].asString());
  if (Path.empty()) {
    trace("ignoring a document that is not a local file: " + TD["uri"].asString());
    return;
  }
  if (Docs.contains(Path))
    (void)send(requests::editorClose(Path));
  else if (!std::binary_search(ProjectFiles.begin(), ProjectFiles.end(), Path))
    ProjectFilesStale = true;
  Document &Doc = Docs[Path];
  Doc.URI = TD["uri"].asString();
  Doc.Path = Path;
  Doc.Text = TD["text"].asString();
  Doc.Version = TD["version"].asInt();
  Doc.Lines = LineTable(Doc.Text);
  (void)send(requests::editorOpen(Path, Doc.Text));
  writeShadow(Doc);
  queueDiagnostics(Path);
}

void Server::didChange(const json::Value &Params) {
  Document *Doc = document(Params);
  if (Doc == nullptr)
    return;
  for (const json::Value &Change : Params["contentChanges"].elements()) {
    ByteOffset Begin{0};
    ByteOffset End{Doc->Text.size()};
    if (Change.has("range")) {
      const DocumentView View = Doc->view(Enc);
      Begin = View.offsetOf(Change["range"]["start"]);
      End = std::max(Begin, View.offsetOf(Change["range"]["end"]));
    }
    const std::string &NewText = Change["text"].asString();
    const std::size_t Length = End.Value - Begin.Value;
    Doc->Text.replace(Begin.Value, Length, NewText);
    Doc->Lines = LineTable(Doc->Text);
    if (!send(requests::editorReplaceText(Doc->Path, Begin, Length, NewText)).OK) {
      // Out of step with sourcekitd somehow: give it the whole text again.
      (void)send(requests::editorClose(Doc->Path));
      (void)send(requests::editorOpen(Doc->Path, Doc->Text));
    }
  }
  Doc->Version = Params["textDocument"]["version"].asInt(Doc->Version);
  writeShadow(*Doc);
  queueDiagnostics(Doc->Path);
}

void Server::didClose(const json::Value &Params) {
  const Document *Doc = document(Params);
  if (Doc == nullptr)
    return;
  (void)send(requests::editorClose(Doc->Path));
  json::Value P = json::Value::object();
  P.set("uri", Doc->URI).set("diagnostics", json::Value::array());
  notify("textDocument/publishDiagnostics", std::move(P));
  if (!Doc->ShadowPath.empty())
    (void)std::remove(Doc->ShadowPath.c_str());
  Docs.erase(Doc->Path);
  OverlayStale = true;
  ProjectFilesStale = true;
}

//===----------------------------------------------------------------------===//
// Diagnostics
//===----------------------------------------------------------------------===//

void Server::queueDiagnostics(const std::string &FirstPath) {
  // An edit can change what every other open file means, so all of them are
  // diagnosed again, the edited one first.
  std::erase(DiagnosticsQueue, FirstPath);
  DiagnosticsQueue.push_front(FirstPath);
  for (const auto &[Path, Doc] : Docs)
    if (std::find(DiagnosticsQueue.begin(), DiagnosticsQueue.end(), Path) == DiagnosticsQueue.end())
      DiagnosticsQueue.push_back(Path);
}

void Server::publishDiagnostics(const Document &Doc) {
  const skd::Reply R = send(requests::diagnostics(Doc.Path, compilerArgs()));
  if (!R.OK)
    return; // keep what was published last
  json::Value P = json::Value::object();
  P.set("uri", Doc.URI)
      .set("version", Doc.Version)
      .set("diagnostics", diagnostics(R.Value, Doc.Path, Doc.view(Enc), Sources));
  notify("textDocument/publishDiagnostics", std::move(P));
}

//===----------------------------------------------------------------------===//
// Requests
//===----------------------------------------------------------------------===//

json::Value Server::completion(const json::Value &Params) {
  const Document *Doc = document(Params);
  if (Doc == nullptr)
    return {};
  const DocumentView View = Doc->view(Enc);
  CompletionRequest Request;
  Request.Cursor = View.offsetOf(Params["position"]);
  // sourcekitd completes at the start of the word being typed and leaves
  // filtering to its client, as SourceKit-LSP does it.
  Request.WordStart = identifierStart(Doc->Text, Request.Cursor);
  Request.Snippets = SnippetSupport;
  Request.MaxItems = Opts.MaxCompletions;
  const skd::Reply R =
      send(requests::codeComplete(Doc->Path, Doc->Text, Request.WordStart, compilerArgs()));
  return R.OK ? completionList(R.Value, View, Request) : json::Value();
}

json::Value Server::hover(const json::Value &Params) {
  const Document *Doc = document(Params);
  if (Doc == nullptr)
    return {};
  const DocumentView View = Doc->view(Enc);
  const ByteOffset At = View.offsetOf(Params["position"]);
  const skd::Reply R = send(requests::cursorInfo(Doc->Path, At, compilerArgs()));
  return R.OK ? lite::hover(R.Value, View, At) : json::Value();
}

json::Value Server::definition(const json::Value &Params) {
  const Document *Doc = document(Params);
  if (Doc == nullptr)
    return json::Value::array();
  const ByteOffset At = Doc->view(Enc).offsetOf(Params["position"]);
  const skd::Reply R = send(requests::cursorInfo(Doc->Path, At, compilerArgs()));
  return R.OK ? lite::definition(R.Value, Sources, Enc) : json::Value::array();
}

json::Value Server::references(const json::Value &Params) {
  json::Value Locations = json::Value::array();
  const Document *Doc = document(Params);
  if (Doc == nullptr)
    return Locations;
  const ByteOffset At = Doc->view(Enc).offsetOf(Params["position"]);
  const std::vector<std::string> Args = compilerArgs();
  const skd::Reply Info = send(requests::cursorInfo(Doc->Path, At, Args));
  if (!Info.OK)
    return Locations;
  const std::string &USR = Info.Value["key.usr"].asString();
  const std::string_view Name = baseName(Info.Value["key.name"].asString());
  if (USR.empty() || Name.empty())
    return Locations;
  const std::string &DeclFile = Info.Value["key.filepath"].asString();
  const ByteOffset DeclAt{Info.Value["key.offset"].asSize(SIZE_MAX)};
  const bool IncludeDeclaration = Params["context"]["includeDeclaration"].asBool(true);

  // No index: a file refers to the symbol if one of its occurrences of the
  // name resolves to the same USR. relatedidents then finds the rest of that
  // file's references, semantically.
  std::set<std::pair<std::string, std::size_t>> Seen;
  const std::string DocPath = Doc->Path;
  for (const std::string &Path : ProjectFiles) {
    const std::optional<std::string> Text = Sources.text(Path);
    if (!Text)
      continue;
    std::vector<ByteOffset> Candidates = findIdentifier(*Text, Name);
    if (Path == DocPath)
      Candidates.insert(Candidates.begin(), identifierStart(*Text, At));
    if (Candidates.size() > MaxCandidatesPerFile)
      Candidates.resize(MaxCandidatesPerFile); // a common name; stop early
    for (const ByteOffset Candidate : Candidates) {
      const skd::Reply Here = send(requests::cursorInfo(Path, Candidate, Args));
      if (!Here.OK || !refersTo(Here.Value, USR))
        continue;
      const skd::Reply Related = send(requests::relatedIdents(Path, Candidate, Args));
      std::vector<ByteRange> Ranges =
          Related.OK ? identRanges(Related.Value) : std::vector<ByteRange>{};
      if (Ranges.empty())
        Ranges.push_back({Candidate, {Candidate.Value + Name.size()}});
      for (const ByteRange &R : Ranges) {
        if (!IncludeDeclaration && Path == DeclFile && R.Begin == DeclAt)
          continue;
        if (Seen.insert({Path, R.Begin.Value}).second)
          Locations.push(location(Sources, Enc, Path, R));
      }
      break;
    }
  }
  return Locations;
}

json::Value Server::documentHighlight(const json::Value &Params) {
  const Document *Doc = document(Params);
  if (Doc == nullptr)
    return json::Value::array();
  const DocumentView View = Doc->view(Enc);
  const skd::Reply R =
      send(requests::relatedIdents(Doc->Path, View.offsetOf(Params["position"]), compilerArgs()));
  return R.OK ? highlights(R.Value, View) : json::Value::array();
}

json::Value Server::documentSymbol(const json::Value &Params) {
  const Document *Doc = document(Params);
  if (Doc == nullptr)
    return json::Value::array();
  // A scratch editor document, as SourceKit-LSP does it: asking for the
  // structure of the real one would reset its state.
  const std::string Scratch = "sourcekit-lsp-lite-symbols:" + Doc->Path;
  const skd::Reply R = send(requests::editorOpen(Scratch, Doc->Text, /*Substructure=*/true));
  (void)send(requests::editorClose(Scratch));
  return R.OK ? documentSymbols(R.Value, Doc->view(Enc)) : json::Value::array();
}

json::Value Server::signatureHelp(const json::Value &Params) {
  const Document *Doc = document(Params);
  if (Doc == nullptr)
    return {};
  const ByteOffset At = Doc->view(Enc).offsetOf(Params["position"]);
  const skd::Reply R = send(requests::signatureHelp(Doc->Path, At, compilerArgs()));
  return R.OK ? lite::signatureHelp(R.Value, Enc) : json::Value();
}

} // namespace lite
