// Positions, URIs and the text sourcekitd hands back.

#include "text.h"

#include <algorithm>
#include <array>

namespace lite {

namespace {

/// Length in bytes of the UTF-8 sequence that starts with \p Lead. A stray
/// continuation byte or an invalid lead counts as a sequence of one, so
/// malformed text still advances.
std::size_t sequenceLength(unsigned char Lead) noexcept {
  if (Lead < 0x80U)
    return 1;
  if (Lead >= 0xF8U)
    return 1;
  if (Lead >= 0xF0U)
    return 4;
  if (Lead >= 0xE0U)
    return 3;
  if (Lead >= 0xC0U)
    return 2;
  return 1;
}

/// Length of the sequence at \p I, never past the end of \p Text.
std::size_t sequenceAt(std::string_view Text, std::size_t I) noexcept {
  return std::min(sequenceLength(static_cast<unsigned char>(Text[I])), Text.size() - I);
}

/// Code units of \p Enc for a sequence of \p Bytes UTF-8 bytes.
std::int64_t unitsFor(std::size_t Bytes, Encoding Enc) noexcept {
  if (Enc == Encoding::UTF8)
    return static_cast<std::int64_t>(Bytes);
  return Bytes == 4 ? 2 : 1; // outside the BMP: a surrogate pair
}

char lower(char C) noexcept {
  return (C >= 'A' && C <= 'Z') ? static_cast<char>(C - 'A' + 'a') : C;
}

bool isByte(std::string_view Text, std::size_t I, char C) noexcept {
  return I < Text.size() && Text[I] == C;
}

} // namespace

LineTable::LineTable(std::string_view Text) {
  for (std::size_t I = 0; I < Text.size(); ++I) {
    if (Text[I] == '\r' && isByte(Text, I + 1, '\n'))
      ++I;
    if (Text[I] == '\n' || Text[I] == '\r')
      Starts.push_back(I + 1);
  }
}

std::size_t LineTable::lineEnd(std::string_view Text, std::size_t Line) const noexcept {
  const std::size_t Begin = Starts[Line];
  if (Line + 1 >= Starts.size())
    return Text.size();
  std::size_t End = Starts[Line + 1];
  if (End > Begin && Text[End - 1] == '\n')
    --End;
  if (End > Begin && Text[End - 1] == '\r')
    --End;
  return End;
}

ByteOffset LineTable::offsetOf(std::string_view Text, Position Pos, Encoding Enc) const noexcept {
  if (Pos.Line < 0)
    return {};
  const auto Line = static_cast<std::size_t>(Pos.Line);
  if (Line >= Starts.size())
    return {Text.size()};
  std::size_t I = Starts[Line];
  const std::size_t End = lineEnd(Text, Line);
  std::int64_t Units = 0;
  while (I < End && Units < Pos.Character) {
    const std::size_t Len = std::min(sequenceAt(Text, I), End - I);
    const std::int64_t U = unitsFor(Len, Enc);
    if (Units + U > Pos.Character)
      break; // inside this code point: round down to its start
    Units += U;
    I += Len;
  }
  return {I};
}

Position LineTable::positionOf(std::string_view Text, ByteOffset Offset,
                               Encoding Enc) const noexcept {
  const std::size_t At = std::min(Offset.Value, Text.size());
  // The last line that starts at or before At.
  const auto It = std::upper_bound(Starts.begin(), Starts.end(), At);
  const auto Line = static_cast<std::size_t>(std::distance(Starts.begin(), It) - 1);
  Position Pos{static_cast<std::int64_t>(Line), 0};
  const std::size_t End = std::min(At, lineEnd(Text, Line));
  for (std::size_t I = Starts[Line]; I < End;) {
    const std::size_t Len = sequenceAt(Text, I);
    if (I + Len > End)
      break; // At is inside this sequence: it has not begun yet
    Pos.Character += unitsFor(Len, Enc);
    I += Len;
  }
  return Pos;
}

ByteOffset LineTable::offsetOf(std::string_view Text, LineColumn At) const noexcept {
  if (At.Line < 1)
    return {};
  const auto Line = static_cast<std::size_t>(At.Line - 1);
  if (Line >= Starts.size())
    return {Text.size()};
  const auto Column = static_cast<std::size_t>(std::max<std::int64_t>(At.Column - 1, 0));
  return {std::min(Starts[Line] + Column, lineEnd(Text, Line))};
}

bool isIdentifierByte(unsigned char C) noexcept {
  return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_' ||
         C >= 0x80U;
}

ByteOffset identifierStart(std::string_view Text, ByteOffset At) noexcept {
  std::size_t I = std::min(At.Value, Text.size());
  while (I > 0 && isIdentifierByte(static_cast<unsigned char>(Text[I - 1])))
    --I;
  return {I};
}

ByteOffset identifierEnd(std::string_view Text, ByteOffset At) noexcept {
  std::size_t I = std::min(At.Value, Text.size());
  while (I < Text.size() && isIdentifierByte(static_cast<unsigned char>(Text[I])))
    ++I;
  return {I};
}

std::vector<ByteOffset> findIdentifier(std::string_view Text, std::string_view Word) {
  std::vector<ByteOffset> Found;
  if (Word.empty())
    return Found;
  for (std::size_t At = Text.find(Word); At != std::string_view::npos;
       At = Text.find(Word, At + 1)) {
    const bool StartsWord = At == 0 || !isIdentifierByte(static_cast<unsigned char>(Text[At - 1]));
    const std::size_t End = At + Word.size();
    const bool EndsWord =
        End >= Text.size() || !isIdentifierByte(static_cast<unsigned char>(Text[End]));
    if (StartsWord && EndsWord)
      Found.push_back({At});
  }
  return Found;
}

std::size_t utf16Length(std::string_view Text) noexcept {
  std::size_t Units = 0;
  for (std::size_t I = 0; I < Text.size();) {
    const std::size_t Len = sequenceAt(Text, I);
    Units += Len == 4 ? 2 : 1;
    I += Len;
  }
  return Units;
}

namespace {
int hexDigit(char H) noexcept {
  if (H >= '0' && H <= '9')
    return H - '0';
  if (H >= 'a' && H <= 'f')
    return H - 'a' + 10;
  if (H >= 'A' && H <= 'F')
    return H - 'A' + 10;
  return -1;
}
} // namespace

std::string pathFromURI(std::string_view URI) {
  constexpr std::string_view Scheme = "file://";
  if (!URI.starts_with(Scheme))
    return {};
  std::string_view Rest = URI.substr(Scheme.size());
  // file://host/path: only an empty host or "localhost" names this machine.
  if (Rest.starts_with("localhost/"))
    Rest.remove_prefix(std::string_view("localhost").size());
  if (!Rest.starts_with('/'))
    return {};
  std::string Path;
  for (std::size_t I = 0; I < Rest.size(); ++I) {
    const char C = Rest[I];
    if (C == '?' || C == '#')
      break;
    if (C == '%' && I + 2 < Rest.size()) {
      const int Hi = hexDigit(Rest[I + 1]);
      const int Lo = hexDigit(Rest[I + 2]);
      if (Hi >= 0 && Lo >= 0) {
        Path += static_cast<char>((Hi << 4) | Lo);
        I += 2;
        continue;
      }
    }
    Path += C;
  }
  return Path;
}

std::string uriFromPath(std::string_view Path) {
  static constexpr std::string_view Hex = "0123456789ABCDEF";
  std::string URI = "file://";
  if (!Path.starts_with('/'))
    URI += '/';
  for (const char Ch : Path) {
    const auto C = static_cast<unsigned char>(Ch);
    const bool Plain = (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') ||
                       C == '/' || C == '-' || C == '.' || C == '_' || C == '~';
    if (Plain) {
      URI += Ch;
    } else {
      URI += '%';
      URI += Hex[C >> 4U];
      URI += Hex[C & 0xFU];
    }
  }
  return URI;
}

namespace {
struct Entity {
  std::string_view Name;
  char Char;
};
constexpr std::array<Entity, 5> Entities{
    {{"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}}};

void decodeEntitiesInto(std::string_view S, std::string &Out) {
  for (std::size_t I = 0; I < S.size(); ++I) {
    if (S[I] != '&') {
      Out += S[I];
      continue;
    }
    const auto E = std::find_if(Entities.begin(), Entities.end(), [&](const Entity &En) {
      return S.substr(I).starts_with(En.Name);
    });
    if (E == Entities.end()) {
      Out += '&';
    } else {
      Out += E->Char;
      I += E->Name.size() - 1;
    }
  }
}
} // namespace

std::string stripMarkup(std::string_view XML) {
  std::string Out;
  std::size_t I = 0;
  while (I < XML.size()) {
    const std::size_t Tag = XML.find('<', I);
    if (Tag == std::string_view::npos) {
      decodeEntitiesInto(XML.substr(I), Out);
      break;
    }
    decodeEntitiesInto(XML.substr(I, Tag - I), Out);
    const std::size_t Close = XML.find('>', Tag);
    if (Close == std::string_view::npos)
      break;
    // Paragraph boundaries in documentation XML become blank lines.
    const std::string_view Name = XML.substr(Tag, Close - Tag + 1);
    if (Name == "</Para>" || Name == "</Item>")
      Out += "\n\n";
    I = Close + 1;
  }
  return Out;
}

std::string elementText(std::string_view XML, std::string_view Tag) {
  const std::string Open = "<" + std::string(Tag) + ">";
  const std::string Close = "</" + std::string(Tag) + ">";
  std::size_t Begin = XML.find(Open);
  if (Begin == std::string_view::npos)
    return {};
  Begin += Open.size();
  const std::size_t End = XML.find(Close, Begin);
  if (End == std::string_view::npos)
    return {};
  std::string Text = stripMarkup(XML.substr(Begin, End - Begin));
  while (!Text.empty() && (Text.back() == '\n' || Text.back() == ' '))
    Text.pop_back();
  return Text;
}

namespace {
/// What a placeholder shows: "T##x: Double##Double" shows "x: Double", "name"
/// shows "name".
std::string_view placeholderDisplay(std::string_view Inner) noexcept {
  if (Inner.starts_with("T##")) {
    Inner.remove_prefix(3);
    const std::size_t Sep = Inner.find("##");
    if (Sep != std::string_view::npos)
      Inner = Inner.substr(0, Sep);
  }
  return Inner;
}

void appendSnippetEscaped(std::string_view S, std::string &Out) {
  for (const char C : S) {
    if (C == '$' || C == '}' || C == '\\')
      Out += '\\';
    Out += C;
  }
}
} // namespace

std::string snippetFromPlaceholders(std::string_view Text) {
  std::string Out;
  int Index = 0;
  std::size_t I = 0;
  while (I < Text.size()) {
    const std::size_t Open = Text.find("<#", I);
    const std::size_t Close = Open == std::string_view::npos ? Open : Text.find("#>", Open + 2);
    if (Close == std::string_view::npos) {
      appendSnippetEscaped(Text.substr(I), Out);
      break;
    }
    appendSnippetEscaped(Text.substr(I, Open - I), Out);
    Out += "${" + std::to_string(++Index) + ":";
    appendSnippetEscaped(placeholderDisplay(Text.substr(Open + 2, Close - Open - 2)), Out);
    Out += "}";
    I = Close + 2;
  }
  return Out;
}

std::string plainFromPlaceholders(std::string_view Text) {
  return std::string(Text.substr(0, Text.find("<#")));
}

int matchRank(std::string_view Candidate, std::string_view Typed) noexcept {
  if (Typed.empty() || Candidate.starts_with(Typed))
    return 0;
  if (Candidate.empty() || lower(Candidate[0]) != lower(Typed[0]))
    return -1;
  if (Candidate.size() >= Typed.size() &&
      std::equal(Typed.begin(), Typed.end(), Candidate.begin(),
                 [](char A, char B) { return lower(A) == lower(B); }))
    return 1;
  std::size_t J = 0;
  for (std::size_t I = 0; I < Candidate.size() && J < Typed.size(); ++I)
    if (lower(Candidate[I]) == lower(Typed[J]))
      ++J;
  return J == Typed.size() ? 2 : -1;
}

std::string_view baseName(std::string_view Name) noexcept { return Name.substr(0, Name.find('(')); }

} // namespace lite
