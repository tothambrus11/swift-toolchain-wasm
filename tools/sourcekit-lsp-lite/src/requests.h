// The sourcekitd requests the server sends, built as JSON.
//
// Pure: each function returns the request dictionary as a json::Value whose
// keys are sourcekitd's key names. skd::send turns it into a sourcekitd
// object: "key.request" becomes a uid, every other string stays a string,
// numbers and bools become int64, arrays stay arrays. Building requests as
// data keeps them testable and loggable without sourcekitd.

#pragma once

#include "json.h"
#include "text.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace lite::requests {

/// Opens an editor document. Syntactic only: semantic answers come from
/// requests that carry their own compiler arguments; the editor document is
/// there for its text, which the AST manager prefers to the file on disk.
/// \p Substructure asks for the declaration structure in the response.
[[nodiscard]] json::Value editorOpen(std::string_view Name, std::string_view Text,
                                     bool Substructure = false);

/// Replaces \p Length bytes at \p At in an open editor document.
[[nodiscard]] json::Value editorReplaceText(std::string_view Name, ByteOffset At,
                                            std::size_t Length, std::string_view Text);

[[nodiscard]] json::Value editorClose(std::string_view Name);

/// Semantic and syntactic diagnostics for one file of the compile.
[[nodiscard]] json::Value diagnostics(std::string_view Path, std::span<const std::string> Args);

/// Code completion at \p At, which must be the start of the word being typed
/// (sourcekitd does not filter; the caller does). \p Text is the file's
/// current text.
[[nodiscard]] json::Value codeComplete(std::string_view Path, std::string_view Text, ByteOffset At,
                                       std::span<const std::string> Args);

[[nodiscard]] json::Value cursorInfo(std::string_view Path, ByteOffset At,
                                     std::span<const std::string> Args);

[[nodiscard]] json::Value relatedIdents(std::string_view Path, ByteOffset At,
                                        std::span<const std::string> Args);

[[nodiscard]] json::Value signatureHelp(std::string_view Path, ByteOffset At,
                                        std::span<const std::string> Args);

} // namespace lite::requests
