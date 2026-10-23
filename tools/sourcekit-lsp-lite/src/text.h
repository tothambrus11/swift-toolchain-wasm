// Positions, URIs and the text sourcekitd hands back.
//
// Pure functions of strings, kept apart from the server so they can be tested
// as tables without sourcekitd.
//
// The one thing to get right: LSP counts columns in UTF-16 code units (unless
// client and server agree on UTF-8), sourcekitd counts UTF-8 bytes from the
// start of the buffer. The two are different types here, ByteOffset and
// Position, and LineTable is the only way from one to the other.

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lite {

/// How LSP positions count characters within a line.
enum class Encoding : std::uint8_t { UTF16, UTF8 };

/// A UTF-8 byte offset from the start of a buffer: sourcekitd's key.offset.
struct ByteOffset {
  std::size_t Value = 0;
  friend constexpr auto operator<=>(ByteOffset, ByteOffset) = default;
};

/// An LSP position: 0-based line, and 0-based character in the negotiated
/// Encoding's units.
struct Position {
  std::int64_t Line = 0;
  std::int64_t Character = 0;
  friend constexpr bool operator==(Position, Position) = default;
};

/// A 1-based line and 1-based UTF-8 byte column: how sourcekitd places
/// diagnostics (key.line, key.column).
struct LineColumn {
  std::int64_t Line = 1;
  std::int64_t Column = 1;
};

/// Where each line of a text starts. A line ends at "\n", "\r\n" or "\r", as
/// LSP specifies.
class LineTable {
public:
  LineTable() = default;
  explicit LineTable(std::string_view Text);

  [[nodiscard]] std::size_t lineCount() const noexcept { return Starts.size(); }

  /// The byte offset of an LSP position. A line past the end clamps to the end
  /// of the text, a character past the end of its line clamps to the end of
  /// the line (before its terminator), and a character inside a code point
  /// (between the halves of a surrogate pair) rounds down to its start.
  [[nodiscard]] ByteOffset offsetOf(std::string_view Text, Position Pos,
                                    Encoding Enc) const noexcept;

  /// The LSP position of a byte offset. An offset past the end clamps to the
  /// end; one inside a multi-byte sequence or a line terminator counts as the
  /// end of what precedes it.
  [[nodiscard]] Position positionOf(std::string_view Text, ByteOffset Offset,
                                    Encoding Enc) const noexcept;

  /// The byte offset of a sourcekitd line and column, clamped like offsetOf.
  [[nodiscard]] ByteOffset offsetOf(std::string_view Text, LineColumn At) const noexcept;

private:
  std::vector<std::size_t> Starts{0};
  [[nodiscard]] std::size_t lineEnd(std::string_view Text, std::size_t Line) const noexcept;
};

/// Whether a byte can be part of a Swift identifier. Every byte of a non-ASCII
/// UTF-8 sequence counts, which over-approximates in the direction that is
/// harmless for finding where a word starts and ends.
[[nodiscard]] bool isIdentifierByte(unsigned char C) noexcept;

/// Where the identifier containing or ending at \p At starts.
[[nodiscard]] ByteOffset identifierStart(std::string_view Text, ByteOffset At) noexcept;
/// Where the identifier containing or starting at \p At ends.
[[nodiscard]] ByteOffset identifierEnd(std::string_view Text, ByteOffset At) noexcept;

/// Every offset at which \p Word occurs in \p Text as a whole identifier.
[[nodiscard]] std::vector<ByteOffset> findIdentifier(std::string_view Text, std::string_view Word);

/// The number of UTF-16 code units in UTF-8 text.
[[nodiscard]] std::size_t utf16Length(std::string_view Text) noexcept;

/// file:///a%20b/c.swift -> "/a b/c.swift"; empty for a URI that is not a
/// local file.
[[nodiscard]] std::string pathFromURI(std::string_view URI);
/// "/a b/c.swift" -> file:///a%20b/c.swift
[[nodiscard]] std::string uriFromPath(std::string_view Path);

/// Removes XML tags and decodes the five XML entities: sourcekitd's
/// key.annotated_decl, as text.
[[nodiscard]] std::string stripMarkup(std::string_view XML);
/// The text of the first <Tag>...</Tag> in \p XML, without markup, or empty.
[[nodiscard]] std::string elementText(std::string_view XML, std::string_view Tag);

/// sourcekitd's completion text "moved(by: <#T##Double#>, <#T##Double#>)" as
/// an LSP snippet "moved(by: ${1:Double}, ${2:Double})", with dollar signs,
/// closing braces and backslashes escaped.
[[nodiscard]] std::string snippetFromPlaceholders(std::string_view Text);
/// The same for a client without snippets: the text before the first
/// placeholder, "moved(by: ".
[[nodiscard]] std::string plainFromPlaceholders(std::string_view Text);

/// Whether \p Candidate matches what was typed: a case-insensitive subsequence
/// whose first character matches. Returns a rank, lower is better (0 prefix
/// with the same case, 1 prefix ignoring case, 2 subsequence), or -1 for no
/// match. Nothing typed matches everything with rank 0.
[[nodiscard]] int matchRank(std::string_view Candidate, std::string_view Typed) noexcept;

/// The name before its argument labels: "moved(by:_:)" -> "moved".
[[nodiscard]] std::string_view baseName(std::string_view Name) noexcept;

} // namespace lite
