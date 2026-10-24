// sourcekitd responses, as JSON (skd.h), turned into LSP results.
//
// Pure: every function here is a function of a response and the text it is
// about, so tests run them on recorded responses (tests/fixtures) without
// sourcekitd. The mapping follows SourceKit-LSP's where it has one.

#pragma once

#include "json.h"
#include "text.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lite {

/// A half-open byte range in one buffer.
struct ByteRange {
  ByteOffset Begin;
  ByteOffset End;
  friend constexpr bool operator==(ByteRange, ByteRange) = default;
};

/// One document's text as positions are computed in it. Does not own the text
/// or the line table; both must outlive it.
class DocumentView {
public:
  DocumentView(std::string_view Source, const LineTable &Table, Encoding Units) noexcept
      : Text(Source), Lines(&Table), Enc(Units) {}

  [[nodiscard]] std::string_view text() const noexcept { return Text; }
  [[nodiscard]] Encoding encoding() const noexcept { return Enc; }
  /// The byte offset of an LSP {line, character} object.
  [[nodiscard]] ByteOffset offsetOf(const json::Value &LSPPosition) const noexcept;
  [[nodiscard]] ByteOffset offsetOf(LineColumn At) const noexcept;
  [[nodiscard]] json::Value position(ByteOffset At) const;
  [[nodiscard]] json::Value range(ByteRange R) const;

private:
  std::string_view Text;
  const LineTable *Lines;
  Encoding Enc;
};

/// Where the server finds files other than the one a request is about: open
/// documents first, then the file system.
class FileSource {
public:
  FileSource() = default;
  virtual ~FileSource() = default;
  FileSource(const FileSource &) = delete;
  FileSource &operator=(const FileSource &) = delete;
  FileSource(FileSource &&) = delete;
  FileSource &operator=(FileSource &&) = delete;

  /// The current text of \p Path, or nullopt if it cannot be read.
  [[nodiscard]] virtual std::optional<std::string> text(const std::string &Path) = 0;
  /// The URI the client knows \p Path by.
  [[nodiscard]] virtual std::string uri(const std::string &Path) = 0;
};

/// An LSP Location for a byte range in any file. A file that cannot be read
/// gets the range at its start rather than no location.
[[nodiscard]] json::Value location(FileSource &Files, Encoding Enc, const std::string &Path,
                                   ByteRange R);

/// LSP Diagnostic[] for \p Path from a source.request.diagnostics response.
/// Diagnostics sourcekitd attributes to other files are left out; notes
/// become relatedInformation. The compiler's fix-its become quick fixes in
/// each diagnostic's `codeActions` (clangd's extension, which SourceKit-LSP
/// uses too): the diagnostic's own fix-its as one preferred action, and each
/// note's as an action titled by the note. codeActions() hands them back
/// from the diagnostics a client sends with textDocument/codeAction, so the
/// edits are always those computed with the text the client saw.
[[nodiscard]] json::Value diagnostics(const json::Value &Response, const std::string &Path,
                                      const DocumentView &Doc, FileSource &Files);

struct CompletionRequest {
  ByteOffset WordStart; ///< where codecomplete was asked
  ByteOffset Cursor;    ///< where the client asked
  bool Snippets = false;
  std::size_t MaxItems = 200;
};

/// An LSP CompletionList from a source.request.codecomplete response,
/// filtered by the text between WordStart and Cursor, ranked, and capped
/// (isIncomplete when capped).
[[nodiscard]] json::Value completionList(const json::Value &Response, const DocumentView &Doc,
                                         const CompletionRequest &Request);

/// An LSP Hover from a cursorinfo response (fenced declaration, then the doc
/// comment), or null when cursorinfo found nothing.
[[nodiscard]] json::Value hover(const json::Value &Response, const DocumentView &Doc,
                                ByteOffset At);

/// LSP Location[] from a cursorinfo response: the declaration, or nothing for
/// a declaration without a file (the standard library).
[[nodiscard]] json::Value definition(const json::Value &Response, FileSource &Files, Encoding Enc);

/// The ranges of a relatedidents response.
[[nodiscard]] std::vector<ByteRange> identRanges(const json::Value &Response);

/// LSP DocumentHighlight[] from a relatedidents response.
[[nodiscard]] json::Value highlights(const json::Value &Response, const DocumentView &Doc);

/// LSP DocumentSymbol[] from an editor.open response with substructure.
[[nodiscard]] json::Value documentSymbols(const json::Value &Response, const DocumentView &Doc);

/// An LSP SignatureHelp from a signaturehelp response, or null when there is
/// no call around the cursor.
[[nodiscard]] json::Value signatureHelp(const json::Value &Response, Encoding Enc);

/// Whether a cursorinfo response names \p USR, as its symbol or as one of its
/// secondary symbols (Point(x:y:) is the struct and its initializer).
[[nodiscard]] bool refersTo(const json::Value &CursorInfo, std::string_view USR);

/// What a rename at the cursor would change: the declaration's USR and its
/// base name, or, when Refusal is not empty, why nothing can be renamed there
/// (a sentence for the user; USR and Name are then empty).
struct RenameTarget {
  std::string USR;
  std::string Name;
  std::string Refusal;
};

/// The symbol a rename of \p Word, the identifier under the cursor, would
/// change, from a cursorinfo response at it. The symbol is the primary one or
/// a secondary one whose base name is \p Word (at `Point(x:y:)` that is the
/// struct, not its initializer). Refused: no symbol, a symbol declared
/// outside the project's files (the standard library), modules,
/// initializers, subscripts and operators.
[[nodiscard]] RenameTarget renameTarget(const json::Value &CursorInfo, std::string_view Word);

/// Why \p NewName cannot replace a base name, or empty when it can. It must be
/// one identifier, or a backticked one; a keyword must be backticked.
/// Argument labels are refused: relatedidents reports base-name ranges only,
/// because finding the labels of each call needs swift-syntax, which the wasm
/// sourcekitd is built without (SWIFT_BUILD_SWIFT_SYNTAX=OFF).
[[nodiscard]] std::string renameNameProblem(std::string_view NewName);

/// One place a symbol's name is written.
struct Occurrence {
  std::string Path;
  ByteRange Range;
};

/// An LSP WorkspaceEdit ({"changes": {uri: TextEdit[]}}) writing \p NewName
/// over each occurrence. An occurrence whose text is not \p OldName (or
/// `OldName` in backticks) is left alone rather than overwritten: a range
/// sourcekitd reports for something else (an implicit member, a stale file)
/// must not destroy text. Edits within a file are in document order.
[[nodiscard]] json::Value renameEdit(std::span<const Occurrence> Occurrences,
                                     std::string_view OldName, std::string_view NewName,
                                     FileSource &Files, Encoding Enc);

/// LSP InlayHint[] from a source.request.variable.type response: ": Type"
/// after each variable whose type is inferred, none for annotated ones.
[[nodiscard]] json::Value inlayHints(const json::Value &Response, const DocumentView &Doc);

/// LSP CodeAction[] for a textDocument/codeAction request: the quick fixes
/// carried by the diagnostics in its context (see diagnostics()), each once.
/// Nothing when the client asks only for kinds other than quickfix.
[[nodiscard]] json::Value codeActions(const json::Value &Params);

/// LSP CompletionItemKind for a sourcekitd kind uid.
[[nodiscard]] int completionKind(std::string_view Kind) noexcept;
/// LSP SymbolKind for a declaration kind uid, or 0 for what is not an outline
/// entry (expressions, statements, parameters, locals).
[[nodiscard]] int symbolKind(std::string_view Kind) noexcept;
/// LSP DiagnosticSeverity for a sourcekitd severity uid.
[[nodiscard]] int severity(std::string_view Severity) noexcept;

} // namespace lite
