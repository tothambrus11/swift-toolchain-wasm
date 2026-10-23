// What the server is told about the project.

#include "options.h"

#include "text.h"

#include <optional>
#include <set>

namespace lite {

std::string_view usage() noexcept {
  return "usage: sourcekit-lsp-lite [options] [-- compiler arguments...]\n"
         "\n"
         "Speaks LSP on stdin/stdout (Content-Length framing), answered by the\n"
         "in-process sourcekitd.\n"
         "\n"
         "  --root DIR             project root (default: initialize's rootUri)\n"
         "  --module-name NAME     module the project compiles as (default:\n"
         "                         the one @testable import names, else main)\n"
         "  --exclude PREFIX       leave out project files under PREFIX, relative\n"
         "                         to the root, unless open (repeatable)\n"
         "  --file PATH            compile exactly these files (repeatable;\n"
         "                         default: every *.swift under the root)\n"
         "  --no-scan              only open documents are project files\n"
         "  --shadow-dir DIR|none  where unsaved text is mirrored for -vfsoverlay\n"
         "                         (default: a new directory under $TMPDIR)\n"
         "  --max-completions N    cap on completion items (default 200)\n"
         "  --log                  log messages and latencies to stderr\n"
         "  --help                 this text\n"
         "  --                     the rest are compiler arguments, e.g. -sdk,\n"
         "                         -target, -resource-dir, -module-cache-path\n"
         "\n"
         "initializationOptions may say the same: compilerArgs, moduleName,\n"
         "exclude, files, scan, shadowDirectory, maxCompletions, log.\n";
}

namespace {
/// A decimal count, or nullopt for anything else (or more than nine digits).
std::optional<std::size_t> parseCount(std::string_view S) {
  if (S.empty() || S.size() > 9)
    return std::nullopt;
  std::size_t N = 0;
  for (const char C : S) {
    if (C < '0' || C > '9')
      return std::nullopt;
    N = N * 10 + static_cast<std::size_t>(C - '0');
  }
  return N;
}
} // namespace

std::string parseArguments(std::span<const std::string> Args, Options &Opts) {
  for (std::size_t I = 0; I < Args.size(); ++I) {
    const std::string &A = Args[I];
    const auto Next = [&](std::string &Out) {
      if (I + 1 >= Args.size())
        return false;
      Out = Args[++I];
      return true;
    };
    std::string V;
    if (A == "--") {
      Opts.CompilerArgs.insert(Opts.CompilerArgs.end(),
                               Args.begin() + static_cast<std::ptrdiff_t>(I) + 1, Args.end());
      break;
    }
    if (A == "--root") {
      if (!Next(Opts.Root))
        return "--root needs a directory";
    } else if (A == "--module-name") {
      if (!Next(Opts.ModuleName))
        return "--module-name needs a name";
    } else if (A == "--exclude") {
      if (!Next(V))
        return "--exclude needs a path prefix";
      Opts.Exclude.push_back(V);
    } else if (A == "--file") {
      if (!Next(V))
        return "--file needs a path";
      Opts.Files.push_back(V);
    } else if (A == "--no-scan") {
      Opts.Scan = false;
    } else if (A == "--shadow-dir") {
      if (!Next(Opts.ShadowDirectory))
        return "--shadow-dir needs a directory, or none";
    } else if (A == "--max-completions") {
      const std::optional<std::size_t> N = Next(V) ? parseCount(V) : std::nullopt;
      if (!N || *N == 0)
        return "--max-completions needs a positive number";
      Opts.MaxCompletions = *N;
    } else if (A == "--log") {
      Opts.Log = true;
    } else if (A == "--stdio") {
      // Accepted for clients that always pass it; stdio is the transport.
    } else {
      return "unknown argument: " + A;
    }
  }
  return {};
}

namespace {
void appendStrings(const json::Value &V, std::vector<std::string> &Out) {
  for (const json::Value &E : V.elements())
    if (E.isString())
      Out.push_back(E.asString());
}
} // namespace

void applyInitializationOptions(const json::Value &Init, Options &Opts) {
  if (!Init.isObject())
    return;
  appendStrings(Init["compilerArgs"], Opts.CompilerArgs);
  appendStrings(Init["exclude"], Opts.Exclude);
  appendStrings(Init["files"], Opts.Files);
  if (Init["root"].isString())
    Opts.Root = Init["root"].asString();
  if (Init["moduleName"].isString())
    Opts.ModuleName = Init["moduleName"].asString();
  if (Init["scan"].isBool())
    Opts.Scan = Init["scan"].asBool();
  if (Init["shadowDirectory"].isString())
    Opts.ShadowDirectory = Init["shadowDirectory"].asString();
  if (Init["maxCompletions"].asInt() > 0)
    Opts.MaxCompletions = Init["maxCompletions"].asSize();
  if (Init["log"].asBool())
    Opts.Log = true;
}

std::string inferModuleName(std::span<const std::string> Sources) {
  constexpr std::string_view Needle = "@testable import ";
  std::set<std::string, std::less<>> Names;
  for (const std::string &Text : Sources) {
    for (std::size_t At = Text.find(Needle); At != std::string::npos;
         At = Text.find(Needle, At + 1)) {
      std::size_t B = At + Needle.size();
      while (B < Text.size() && Text[B] == ' ')
        ++B;
      const ByteOffset E = identifierEnd(Text, {B});
      if (E.Value > B)
        Names.insert(Text.substr(B, E.Value - B));
    }
  }
  return Names.size() == 1 ? *Names.begin() : "main";
}

} // namespace lite
