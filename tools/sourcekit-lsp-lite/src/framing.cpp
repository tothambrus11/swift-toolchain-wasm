// LSP base protocol framing.

#include "framing.h"

#include <algorithm>

namespace lite {

namespace {
constexpr std::string_view HeaderEnd = "\r\n\r\n";
constexpr std::string_view LengthName = "content-length";
/// A message larger than this is refused rather than buffered: nothing an
/// editor sends for one Swift project comes close.
constexpr std::size_t MaxMessage = std::size_t{1} << 28U;

char lower(char C) noexcept {
  return (C >= 'A' && C <= 'Z') ? static_cast<char>(C - 'A' + 'a') : C;
}

std::string_view trim(std::string_view S) noexcept {
  while (!S.empty() && (S.front() == ' ' || S.front() == '\t'))
    S.remove_prefix(1);
  while (!S.empty() && (S.back() == ' ' || S.back() == '\t'))
    S.remove_suffix(1);
  return S;
}
} // namespace

std::optional<std::size_t> contentLength(std::string_view Headers) {
  while (!Headers.empty()) {
    const std::size_t EOL = Headers.find("\r\n");
    const std::string_view Line = Headers.substr(0, EOL);
    Headers = EOL == std::string_view::npos ? std::string_view() : Headers.substr(EOL + 2);
    const std::size_t Colon = Line.find(':');
    if (Colon == std::string_view::npos)
      continue;
    const std::string_view Name = trim(Line.substr(0, Colon));
    if (Name.size() != LengthName.size() ||
        !std::equal(Name.begin(), Name.end(), LengthName.begin(),
                    [](char A, char B) { return lower(A) == B; }))
      continue;
    const std::string_view Value = trim(Line.substr(Colon + 1));
    if (Value.empty() || Value.size() > 12)
      return std::nullopt;
    std::size_t N = 0;
    for (const char C : Value) {
      if (C < '0' || C > '9')
        return std::nullopt;
      N = N * 10 + static_cast<std::size_t>(C - '0');
    }
    return N;
  }
  return std::nullopt;
}

void FrameDecoder::feed(std::string_view Bytes) { Buffer.append(Bytes); }

void FrameDecoder::compact() {
  // Drop consumed bytes once they are most of the buffer, so a long session
  // neither grows without bound nor copies on every message.
  if (Consumed > 0 && Consumed * 2 >= Buffer.size()) {
    Buffer.erase(0, Consumed);
    Consumed = 0;
  }
}

std::optional<std::string> FrameDecoder::next() {
  while (true) {
    const std::string_view Pending = std::string_view(Buffer).substr(Consumed);
    const std::size_t End = Pending.find(HeaderEnd);
    if (End == std::string_view::npos)
      return std::nullopt;
    const std::optional<std::size_t> Length = contentLength(Pending.substr(0, End));
    const std::size_t BodyStart = End + HeaderEnd.size();
    if (!Length || *Length > MaxMessage) {
      Error = Length ? "message too large" : "header block without Content-Length";
      Consumed += BodyStart;
      compact();
      continue;
    }
    if (Pending.size() - BodyStart < *Length)
      return std::nullopt;
    std::string Body(Pending.substr(BodyStart, *Length));
    Consumed += BodyStart + *Length;
    compact();
    return Body;
  }
}

std::string frame(std::string_view Body) {
  std::string Out = "Content-Length: " + std::to_string(Body.size()) + "\r\n\r\n";
  Out.append(Body);
  return Out;
}

} // namespace lite
