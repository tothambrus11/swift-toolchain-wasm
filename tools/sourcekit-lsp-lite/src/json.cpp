// The JSON that LSP needs, and no more.

#include "json.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace lite::json {

namespace {
const Value &nullValue() noexcept {
  static const Value Null;
  return Null;
}
const std::string &emptyString() noexcept {
  static const std::string Empty;
  return Empty;
}
const std::vector<Value> &emptyArray() noexcept {
  static const std::vector<Value> Empty;
  return Empty;
}
const std::vector<Member> &emptyObject() noexcept {
  static const std::vector<Member> Empty;
  return Empty;
}
} // namespace

std::int64_t Value::asInt(std::int64_t Default) const noexcept {
  if (!isNumber())
    return Default;
  return IsInt ? Int : static_cast<std::int64_t>(Num);
}

std::size_t Value::asSize(std::size_t Default) const noexcept {
  if (!isNumber())
    return Default;
  const std::int64_t I = asInt();
  return I < 0 ? Default : static_cast<std::size_t>(I);
}

double Value::asDouble(double Default) const noexcept {
  if (!isNumber())
    return Default;
  return IsInt ? static_cast<double>(Int) : Num;
}

const std::string &Value::asString() const noexcept { return isString() ? Str : emptyString(); }

const std::vector<Value> &Value::elements() const noexcept {
  return isArray() ? Arr : emptyArray();
}

const Value &Value::operator[](std::size_t I) const noexcept {
  return isArray() && I < Arr.size() ? Arr[I] : nullValue();
}

Value &Value::push(Value V) {
  if (K == Kind::Null)
    K = Kind::Array;
  Arr.push_back(std::move(V));
  return Arr.back();
}

const std::vector<Member> &Value::members() const noexcept {
  return isObject() ? Obj : emptyObject();
}

const Value &Value::operator[](std::string_view Key) const noexcept {
  if (isObject())
    for (const Member &M : Obj)
      if (M.Key == Key)
        return M.Val;
  return nullValue();
}

bool Value::has(std::string_view Key) const noexcept {
  if (isObject())
    for (const Member &M : Obj)
      if (M.Key == Key)
        return true;
  return false;
}

Value &Value::set(std::string_view Key, Value V) {
  if (K == Kind::Null)
    K = Kind::Object;
  for (Member &M : Obj)
    if (M.Key == Key) {
      M.Val = std::move(V);
      return *this;
    }
  Obj.push_back(Member{std::string(Key), std::move(V)});
  return *this;
}

bool operator==(const Value &A, const Value &B) {
  if (A.K != B.K)
    return false;
  switch (A.K) {
  case Value::Kind::Null:
    return true;
  case Value::Kind::Bool:
    return A.Bool == B.Bool;
  case Value::Kind::Number:
    if (A.IsInt && B.IsInt)
      return A.Int == B.Int;
    // Exact comparison is what equality of two parsed numbers means here.
    return A.asDouble() == B.asDouble(); // NOLINT(clang-diagnostic-float-equal)
  case Value::Kind::String:
    return A.Str == B.Str;
  case Value::Kind::Array:
    return A.Arr == B.Arr;
  case Value::Kind::Object:
    if (A.Obj.size() != B.Obj.size())
      return false;
    return std::all_of(A.Obj.begin(), A.Obj.end(),
                       [&B](const Member &M) { return B.has(M.Key) && B[M.Key] == M.Val; });
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Parsing
//===----------------------------------------------------------------------===//

namespace {

/// Nesting deeper than this is refused rather than risking the stack, which on
/// wasm is a fixed-size region of linear memory.
constexpr int MaxDepth = 256;

constexpr std::uint32_t ReplacementCharacter = 0xFFFD;

int hexDigit(char H) noexcept {
  if (H >= '0' && H <= '9')
    return H - '0';
  if (H >= 'a' && H <= 'f')
    return H - 'a' + 10;
  if (H >= 'A' && H <= 'F')
    return H - 'A' + 10;
  return -1;
}

void appendUTF8(std::uint32_t CP, std::string &Out) {
  auto Byte = [&Out](std::uint32_t B) { Out += static_cast<char>(B); };
  if (CP < 0x80) {
    Byte(CP);
  } else if (CP < 0x800) {
    Byte(0xC0U | (CP >> 6U));
    Byte(0x80U | (CP & 0x3FU));
  } else if (CP < 0x10000) {
    Byte(0xE0U | (CP >> 12U));
    Byte(0x80U | ((CP >> 6U) & 0x3FU));
    Byte(0x80U | (CP & 0x3FU));
  } else {
    Byte(0xF0U | (CP >> 18U));
    Byte(0x80U | ((CP >> 12U) & 0x3FU));
    Byte(0x80U | ((CP >> 6U) & 0x3FU));
    Byte(0x80U | (CP & 0x3FU));
  }
}

class Parser {
public:
  explicit Parser(std::string_view Text) noexcept : T(Text) {}

  std::optional<Value> parseDocument(std::string *Error) {
    std::optional<Value> V = parseValue(0);
    if (V) {
      skipSpace();
      if (P != T.size())
        fail("trailing characters after the JSON value");
    }
    if (!Err.empty()) {
      if (Error != nullptr)
        *Error = Err + " at byte " + std::to_string(P);
      return std::nullopt;
    }
    return V;
  }

private:
  std::string_view T;
  std::size_t P = 0;
  std::string Err;

  std::nullopt_t fail(const char *Message) {
    if (Err.empty())
      Err = Message;
    return std::nullopt;
  }

  void skipSpace() noexcept {
    while (P < T.size() && (T[P] == ' ' || T[P] == '\t' || T[P] == '\n' || T[P] == '\r'))
      ++P;
  }

  bool consume(std::string_view Word) noexcept {
    if (T.substr(P, Word.size()) != Word)
      return false;
    P += Word.size();
    return true;
  }

  std::optional<Value> parseValue(int Depth) {
    if (Depth > MaxDepth)
      return fail("nested too deeply");
    skipSpace();
    if (P >= T.size())
      return fail("unexpected end of input");
    const char C = T[P];
    if (C == '{')
      return parseObject(Depth);
    if (C == '[')
      return parseArray(Depth);
    if (C == '"') {
      std::string S;
      if (!parseString(S))
        return std::nullopt;
      return Value(std::move(S));
    }
    if (consume("true"))
      return Value(true);
    if (consume("false"))
      return Value(false);
    if (consume("null"))
      return Value();
    if (C == '-' || (C >= '0' && C <= '9'))
      return parseNumber();
    return fail("unexpected character");
  }

  std::optional<Value> parseObject(int Depth) {
    ++P; // {
    Value Obj = Value::object();
    skipSpace();
    if (P < T.size() && T[P] == '}') {
      ++P;
      return Obj;
    }
    while (true) {
      skipSpace();
      if (P >= T.size() || T[P] != '"')
        return fail("expected a member name");
      std::string Key;
      if (!parseString(Key))
        return std::nullopt;
      skipSpace();
      if (P >= T.size() || T[P] != ':')
        return fail("expected ':'");
      ++P;
      std::optional<Value> V = parseValue(Depth + 1);
      if (!V)
        return std::nullopt;
      Obj.set(Key, std::move(*V));
      skipSpace();
      if (P < T.size() && T[P] == ',') {
        ++P;
        continue;
      }
      if (P < T.size() && T[P] == '}') {
        ++P;
        return Obj;
      }
      return fail("expected ',' or '}'");
    }
  }

  std::optional<Value> parseArray(int Depth) {
    ++P; // [
    Value Arr = Value::array();
    skipSpace();
    if (P < T.size() && T[P] == ']') {
      ++P;
      return Arr;
    }
    while (true) {
      std::optional<Value> V = parseValue(Depth + 1);
      if (!V)
        return std::nullopt;
      Arr.push(std::move(*V));
      skipSpace();
      if (P < T.size() && T[P] == ',') {
        ++P;
        continue;
      }
      if (P < T.size() && T[P] == ']') {
        ++P;
        return Arr;
      }
      return fail("expected ',' or ']'");
    }
  }

  bool parseHex4(std::uint32_t &Out) {
    if (P + 4 > T.size()) {
      fail("truncated \\u escape");
      return false;
    }
    Out = 0;
    for (int I = 0; I < 4; ++I) {
      const int D = hexDigit(T[P++]);
      if (D < 0) {
        fail("bad hex digit in \\u escape");
        return false;
      }
      Out = (Out << 4U) | static_cast<std::uint32_t>(D);
    }
    return true;
  }

  bool parseUnicodeEscape(std::string &Out) {
    std::uint32_t CP = 0;
    if (!parseHex4(CP))
      return false;
    if (CP >= 0xD800 && CP < 0xDC00 && T.substr(P, 2) == "\\u") {
      const std::size_t Save = P;
      P += 2;
      std::uint32_t Low = 0;
      if (!parseHex4(Low))
        return false;
      if (Low >= 0xDC00 && Low < 0xE000)
        CP = 0x10000 + ((CP - 0xD800) << 10U) + (Low - 0xDC00);
      else
        P = Save; // a lone high surrogate; the next escape stands alone
    }
    // A lone surrogate cannot be UTF-8; write U+FFFD, as browsers do.
    if (CP >= 0xD800 && CP < 0xE000)
      CP = ReplacementCharacter;
    appendUTF8(CP, Out);
    return true;
  }

  bool parseString(std::string &Out) {
    ++P; // "
    while (true) {
      // Copy the run up to the next quote or escape in one go: source texts
      // arrive as one long string, and most of it needs no decoding.
      const std::size_t Start = P;
      while (P < T.size() && T[P] != '"' && T[P] != '\\')
        ++P;
      Out.append(T.substr(Start, P - Start));
      if (P >= T.size()) {
        fail("unterminated string");
        return false;
      }
      if (T[P] == '"') {
        ++P;
        return true;
      }
      ++P; // backslash
      if (P >= T.size()) {
        fail("unterminated escape");
        return false;
      }
      const char E = T[P++];
      switch (E) {
      case '"':
        Out += '"';
        break;
      case '\\':
        Out += '\\';
        break;
      case '/':
        Out += '/';
        break;
      case 'b':
        Out += '\b';
        break;
      case 'f':
        Out += '\f';
        break;
      case 'n':
        Out += '\n';
        break;
      case 'r':
        Out += '\r';
        break;
      case 't':
        Out += '\t';
        break;
      case 'u':
        if (!parseUnicodeEscape(Out))
          return false;
        break;
      default:
        fail("unknown escape");
        return false;
      }
    }
  }

  std::optional<Value> parseNumber() {
    const std::size_t Start = P;
    if (T[P] == '-')
      ++P;
    bool Integral = true;
    while (P < T.size()) {
      const char C = T[P];
      if (C >= '0' && C <= '9') {
        ++P;
      } else if (C == '.' || C == 'e' || C == 'E' || C == '+' || C == '-') {
        Integral = false;
        ++P;
      } else {
        break;
      }
    }
    const std::string Digits(T.substr(Start, P - Start));
    char *End = nullptr;
    constexpr std::size_t SafeDigits = 18; // always fits an int64
    if (Integral && Digits.size() <= SafeDigits) {
      const long long I = std::strtoll(Digits.c_str(), &End, 10);
      if (End != nullptr && *End == '\0')
        return Value(static_cast<std::int64_t>(I));
    }
    const double D = std::strtod(Digits.c_str(), &End);
    if (End == nullptr || *End != '\0')
      return fail("malformed number");
    return Value(D);
  }
};

} // namespace

std::optional<Value> parse(std::string_view Text, std::string *Error) {
  return Parser(Text).parseDocument(Error);
}

//===----------------------------------------------------------------------===//
// Serializing
//===----------------------------------------------------------------------===//

void appendQuoted(std::string_view S, std::string &Out) {
  static constexpr std::string_view Hex = "0123456789abcdef";
  Out += '"';
  std::size_t Run = 0;
  for (std::size_t I = 0; I < S.size(); ++I) {
    const auto C = static_cast<unsigned char>(S[I]);
    if (C >= 0x20 && C != '"' && C != '\\')
      continue;
    Out.append(S.substr(Run, I - Run));
    Run = I + 1;
    switch (C) {
    case '"':
      Out += "\\\"";
      break;
    case '\\':
      Out += "\\\\";
      break;
    case '\n':
      Out += "\\n";
      break;
    case '\r':
      Out += "\\r";
      break;
    case '\t':
      Out += "\\t";
      break;
    case '\b':
      Out += "\\b";
      break;
    case '\f':
      Out += "\\f";
      break;
    default:
      Out += "\\u00";
      Out += Hex[C >> 4U];
      Out += Hex[C & 0xFU];
    }
  }
  Out.append(S.substr(Run));
  Out += '"';
}

void serializeTo(const Value &V, std::string &Out) {
  switch (V.K) {
  case Value::Kind::Null:
    Out += "null";
    return;
  case Value::Kind::Bool:
    Out += V.Bool ? "true" : "false";
    return;
  case Value::Kind::Number: {
    if (V.IsInt) {
      Out += std::to_string(V.Int);
      return;
    }
    if (!std::isfinite(V.Num)) {
      Out += "null"; // JSON has no NaN or infinity
      return;
    }
    std::array<char, 32> Buf{};
    const int N = std::snprintf(Buf.data(), Buf.size(), "%.17g", V.Num);
    Out.append(Buf.data(), N > 0 ? static_cast<std::size_t>(N) : 0);
    return;
  }
  case Value::Kind::String:
    appendQuoted(V.Str, Out);
    return;
  case Value::Kind::Array: {
    Out += '[';
    bool First = true;
    for (const Value &E : V.Arr) {
      if (!First)
        Out += ',';
      First = false;
      serializeTo(E, Out);
    }
    Out += ']';
    return;
  }
  case Value::Kind::Object: {
    Out += '{';
    bool First = true;
    for (const Member &M : V.Obj) {
      if (!First)
        Out += ',';
      First = false;
      appendQuoted(M.Key, Out);
      Out += ':';
      serializeTo(M.Val, Out);
    }
    Out += '}';
    return;
  }
  }
}

std::string serialize(const Value &V) {
  std::string Out;
  serializeTo(V, Out);
  return Out;
}

} // namespace lite::json
