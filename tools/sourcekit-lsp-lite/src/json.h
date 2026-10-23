// The JSON that LSP needs, and no more.
//
// A JSON value, a parser and a serializer, written for sourcekit-lsp-lite so
// the server depends on nothing beyond the C++ standard library and
// sourcekitd. Errors are reported by return value, never thrown: the server
// is built with -fno-exceptions, like the LLVM and Swift libraries it is
// linked with on wasm32-wasip1.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lite::json {

struct Member;

/// One JSON value. Objects keep their members in insertion order, which keeps
/// output stable and readable; lookups are linear, which is right for the
/// handful of keys an LSP message or a sourcekitd dictionary has.
class Value {
public:
  enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };

  Value() = default;
  // Implicit on purpose: building a message reads like the JSON it makes.
  // Defined below, where Member is complete.
  Value(std::nullptr_t) noexcept; // NOLINT(google-explicit-constructor,hicpp-explicit-conversions)
  Value(bool B) noexcept;         // NOLINT(google-explicit-constructor,hicpp-explicit-conversions)
  Value(int I) noexcept;          // NOLINT(google-explicit-constructor,hicpp-explicit-conversions)
  Value(std::int64_t I) noexcept; // NOLINT(google-explicit-constructor,hicpp-explicit-conversions)
  Value(double D) noexcept;       // NOLINT(google-explicit-constructor,hicpp-explicit-conversions)
  Value(const char *S);           // NOLINT(google-explicit-constructor,hicpp-explicit-conversions)
  Value(std::string S) noexcept;  // NOLINT(google-explicit-constructor,hicpp-explicit-conversions)
  Value(std::string_view S);      // NOLINT(google-explicit-constructor,hicpp-explicit-conversions)

  /// A size or count; JSON numbers are signed, and no LSP size exceeds that.
  [[nodiscard]] static Value count(std::size_t N) noexcept;
  [[nodiscard]] static Value array();
  [[nodiscard]] static Value object();

  [[nodiscard]] Kind kind() const noexcept { return K; }
  [[nodiscard]] bool isNull() const noexcept { return K == Kind::Null; }
  [[nodiscard]] bool isBool() const noexcept { return K == Kind::Bool; }
  [[nodiscard]] bool isNumber() const noexcept { return K == Kind::Number; }
  [[nodiscard]] bool isString() const noexcept { return K == Kind::String; }
  [[nodiscard]] bool isArray() const noexcept { return K == Kind::Array; }
  [[nodiscard]] bool isObject() const noexcept { return K == Kind::Object; }

  /// The value as a bool; anything that is not a bool is \p Default.
  [[nodiscard]] bool asBool(bool Default = false) const noexcept {
    return isBool() ? Bool : Default;
  }
  /// The value as an integer; a non-number is \p Default, a fraction is
  /// truncated toward zero.
  [[nodiscard]] std::int64_t asInt(std::int64_t Default = 0) const noexcept;
  /// The value as a size: a negative or non-number is \p Default.
  [[nodiscard]] std::size_t asSize(std::size_t Default = 0) const noexcept;
  [[nodiscard]] double asDouble(double Default = 0) const noexcept;
  /// The string, or empty for anything that is not a string.
  [[nodiscard]] const std::string &asString() const noexcept;

  /// Array elements; empty for anything that is not an array.
  [[nodiscard]] const std::vector<Value> &elements() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return isArray() ? Arr.size() : 0; }
  /// The element at \p I, or a null value. Never fails.
  [[nodiscard]] const Value &operator[](std::size_t I) const noexcept;
  /// Appends to an array (a null value becomes an empty array first).
  Value &push(Value V);

  /// Object members; empty for anything that is not an object.
  [[nodiscard]] const std::vector<Member> &members() const noexcept;
  /// The member named \p Key, or a null value. Never fails.
  [[nodiscard]] const Value &operator[](std::string_view Key) const noexcept;
  [[nodiscard]] const Value &operator[](const char *Key) const noexcept {
    return (*this)[std::string_view(Key)];
  }
  [[nodiscard]] bool has(std::string_view Key) const noexcept;
  /// Sets (or replaces) a member (a null value becomes an empty object
  /// first), and returns *this for chaining.
  Value &set(std::string_view Key, Value V);

  friend bool operator==(const Value &A, const Value &B);
  friend void serializeTo(const Value &V, std::string &Out);

private:
  Kind K = Kind::Null;
  bool Bool = false;
  bool IsInt = false;
  double Num = 0;
  std::int64_t Int = 0;
  std::string Str;
  std::vector<Value> Arr;
  std::vector<Member> Obj;
};

struct Member {
  std::string Key;
  Value Val;
};

inline Value::Value(std::nullptr_t) noexcept {}
inline Value::Value(bool B) noexcept : K(Kind::Bool), Bool(B) {}
inline Value::Value(int I) noexcept : K(Kind::Number), IsInt(true), Int(I) {}
inline Value::Value(std::int64_t I) noexcept : K(Kind::Number), IsInt(true), Int(I) {}
inline Value::Value(double D) noexcept : K(Kind::Number), Num(D) {}
inline Value::Value(const char *S) : K(Kind::String), Str(S) {}
inline Value::Value(std::string S) noexcept : K(Kind::String), Str(std::move(S)) {}
inline Value::Value(std::string_view S) : K(Kind::String), Str(S) {}

// Defined here, where Member is complete: returning a Value by value
// instantiates its vector<Member>'s destructor.
inline Value Value::count(std::size_t N) noexcept { return {static_cast<std::int64_t>(N)}; }
inline Value Value::array() {
  Value V;
  V.K = Kind::Array;
  return V;
}
inline Value Value::object() {
  Value V;
  V.K = Kind::Object;
  return V;
}

/// Parses one JSON text. On failure returns nullopt and, if \p Error is given,
/// says what was wrong and at which byte.
[[nodiscard]] std::optional<Value> parse(std::string_view Text, std::string *Error = nullptr);

/// Serializes compactly, with no newline anywhere in the output (control
/// characters in strings are escaped), so messages can be newline-separated.
/// Non-ASCII UTF-8 is written as is.
[[nodiscard]] std::string serialize(const Value &V);
void serializeTo(const Value &V, std::string &Out);

/// Appends \p S as a JSON string literal, quotes included.
void appendQuoted(std::string_view S, std::string &Out);

} // namespace lite::json
