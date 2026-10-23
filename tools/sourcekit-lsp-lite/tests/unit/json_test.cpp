// JSON: what LSP messages and recorded sourcekitd responses need.

#include "json.h"

#include <doctest/doctest.h>

#include <string>

using lite::json::parse;
using lite::json::serialize;
using lite::json::Value;

TEST_SUITE("json") {
  TEST_CASE("an LSP request parses into the values it names") {
    const auto V = parse(R"({"jsonrpc":"2.0","id":7,"method":"textDocument/hover",)"
                         R"("params":{"position":{"line":3,"character":12}}})");
    REQUIRE(V);
    CHECK(V->isObject());
    CHECK((*V)["id"].asInt() == 7);
    CHECK((*V)["method"].asString() == "textDocument/hover");
    CHECK((*V)["params"]["position"]["character"].asInt() == 12);
  }

  TEST_CASE("missing members and wrong types read as defaults, never fail") {
    const auto V = parse(R"({"a":"text","n":3})");
    REQUIRE(V);
    CHECK((*V)["missing"].isNull());
    CHECK((*V)["missing"]["deeper"][std::size_t{4}].isNull());
    CHECK((*V)["a"].asInt(-1) == -1);
    CHECK((*V)["n"].asString().empty());
    CHECK((*V)["n"].asBool(true));
  }

  TEST_CASE("escapes decode to UTF-8, surrogate pairs included") {
    const auto V = parse(R"(["a\"b\\c\/\n\t", "é", "€", "😀", "\ud800x"])");
    REQUIRE(V);
    CHECK((*V)[std::size_t{0}].asString() == "a\"b\\c/\n\t");
    CHECK((*V)[std::size_t{1}].asString() == "\xc3\xa9");
    CHECK((*V)[std::size_t{2}].asString() == "\xe2\x82\xac");
    CHECK((*V)[std::size_t{3}].asString() == "\xf0\x9f\x98\x80");
    CHECK((*V)[std::size_t{4}].asString() == "\xef\xbf\xbdx"); // a lone surrogate
  }

  TEST_CASE("numbers keep integers exact and fractions as doubles") {
    const auto V = parse("[0, -12, 123456789012345678, 1.5, -2e3, 1e400]");
    REQUIRE(V);
    CHECK((*V)[std::size_t{1}].asInt() == -12);
    CHECK((*V)[std::size_t{2}].asInt() == 123456789012345678);
    CHECK((*V)[std::size_t{3}].asDouble() == doctest::Approx(1.5));
    CHECK((*V)[std::size_t{4}].asInt() == -2000);
    CHECK((*V)[std::size_t{1}].asSize(99) == 99); // negative is no size
  }

  TEST_CASE("malformed text is refused with a position") {
    std::string Error;
    CHECK_FALSE(parse(R"({"a":})", &Error));
    CHECK(Error.find("at byte") != std::string::npos);
    CHECK_FALSE(parse(R"({"a":1} x)", &Error));
    CHECK_FALSE(parse(R"("unterminated)", &Error));
    CHECK_FALSE(parse("[1,]", &Error));
    CHECK_FALSE(parse("", &Error));
    CHECK_FALSE(parse(std::string(1000, '['), &Error));
    CHECK(Error.find("nested too deeply") != std::string::npos);
  }

  TEST_CASE("serializing writes no newline, so messages can be newline-separated") {
    Value V = Value::object();
    V.set("text", "line 1\nline 2\r\n\x01").set("n", 3).set("ok", true).set("none", nullptr);
    const std::string S = serialize(V);
    CHECK(S.find('\n') == std::string::npos);
    CHECK(S == R"({"text":"line 1\nline 2\r\n\u0001","n":3,"ok":true,"none":null})");
  }

  TEST_CASE("what is serialized parses back to the same value") {
    const std::string Text = R"({"a":[1,2.5,"é",{"b":null}],"c":{"d":false,"e":-7},"f":"😀"})";
    const auto V = parse(Text);
    REQUIRE(V);
    const auto Again = parse(serialize(*V));
    REQUIRE(Again);
    CHECK(*Again == *V);
  }

  TEST_CASE("set replaces a member in place and keeps the order") {
    Value V = Value::object();
    V.set("a", 1).set("b", 2).set("a", 3);
    CHECK(serialize(V) == R"({"a":3,"b":2})");
  }
}
