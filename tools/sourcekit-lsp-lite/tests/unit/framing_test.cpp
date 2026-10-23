// LSP base protocol framing, however the stream is split.

#include "framing.h"

#include <doctest/doctest.h>

#include <string>

using namespace lite;

TEST_SUITE("framing") {
  TEST_CASE("a framed message comes back whole") {
    FrameDecoder D;
    D.feed(frame(R"({"id":1})"));
    const auto M = D.next();
    REQUIRE(M);
    CHECK(*M == R"({"id":1})");
    CHECK_FALSE(D.next());
    CHECK(D.buffered() == 0);
  }

  TEST_CASE("a message split at every byte still arrives once, complete") {
    const std::string Stream = frame("{\"a\":\"\xc3\xa9\"}") + frame("{}");
    FrameDecoder D;
    int Messages = 0;
    for (const char C : Stream) {
      D.feed(std::string_view(&C, 1));
      while (const auto M = D.next())
        ++Messages;
    }
    CHECK(Messages == 2);
  }

  TEST_CASE("Content-Length counts bytes, not characters") {
    CHECK(frame("\xc3\xa9").starts_with("Content-Length: 2\r\n\r\n"));
  }

  TEST_CASE("header names are case-insensitive and other headers are ignored") {
    FrameDecoder D;
    D.feed("content-type: application/vscode-jsonrpc; charset=utf-8\r\n"
           "CONTENT-LENGTH:  2 \r\n\r\n{}");
    const auto M = D.next();
    REQUIRE(M);
    CHECK(*M == "{}");
  }

  TEST_CASE("a header block without a length is skipped and said so") {
    FrameDecoder D;
    D.feed("X-Nothing: 1\r\n\r\n");
    D.feed(frame("{}"));
    const auto M = D.next();
    REQUIRE(M);
    CHECK(*M == "{}");
    CHECK(D.lastError() == "header block without Content-Length");
  }

  TEST_CASE("bad lengths are not lengths") {
    CHECK_FALSE(contentLength("Content-Length: -1"));
    CHECK_FALSE(contentLength("Content-Length: 12a"));
    CHECK_FALSE(contentLength("Content-Type: x"));
    CHECK(contentLength("Content-Length: 0") == std::size_t{0});
  }
}
