// What the server is told about the project: command line and
// initializationOptions, which may say the same things.

#pragma once

#include "json.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lite {

struct Options {
  /// The project root. Empty: taken from initialize's rootUri or
  /// workspaceFolders.
  std::string Root;
  /// The module the project's files compile as. Empty: inferred, see
  /// inferModuleName.
  std::string ModuleName;
  /// Appended after the project's files: on a wasm host -target, -sdk,
  /// -resource-dir and -module-cache-path, which natively come from the
  /// toolchain sourcekitd was loaded from.
  std::vector<std::string> CompilerArgs;
  /// Project-relative path prefixes whose files are left out unless open,
  /// e.g. "Tests/" where XCTest is not available.
  std::vector<std::string> Exclude;
  /// When non-empty, exactly these project files (relative to Root, or
  /// absolute) instead of every *.swift under Root.
  std::vector<std::string> Files;
  /// Whether Root is searched for *.swift files that are not open.
  bool Scan = true;
  /// Where the text of open documents is mirrored for -vfsoverlay, so that
  /// compiles which read other files from disk (completion, signature help)
  /// see unsaved edits. Empty: a new directory under $TMPDIR or /tmp. "none":
  /// no overlay; other files are read as saved.
  std::string ShadowDirectory;
  /// At most this many completion items; the list says isIncomplete when more
  /// matched.
  std::size_t MaxCompletions = 200;
  /// Log each message, each sourcekitd request and their latencies to stderr.
  bool Log = false;
};

/// Applies command-line arguments (without argv[0]) to \p Opts. Returns what
/// was wrong, or empty. Everything after "--" is a compiler argument.
[[nodiscard]] std::string parseArguments(std::span<const std::string> Args, Options &Opts);

/// Applies initialize's initializationOptions: compilerArgs (appended),
/// exclude and files (appended), moduleName, scan, shadowDirectory,
/// maxCompletions, log. Members of the wrong type are ignored.
void applyInitializationOptions(const json::Value &Init, Options &Opts);

/// The module name for a project whose sources say "@testable import X": if
/// exactly one name is imported that way, that name (the tests and the code
/// they test are compiled as one module here, and the compiler accepts
/// importing the module being compiled with a warning); else "main".
[[nodiscard]] std::string inferModuleName(std::span<const std::string> Sources);

[[nodiscard]] std::string_view usage() noexcept;

} // namespace lite
