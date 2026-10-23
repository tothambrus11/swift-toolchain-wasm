// The sourcekitd requests, as the data skd::Session::send turns into
// sourcekitd objects.

#include "requests.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace lite;

TEST_SUITE("requests") {
  const std::vector<std::string> Args{"/p/main.swift", "/p/Geometry.swift", "-module-name", "App"};

  TEST_CASE("editor.open is syntactic only and asks for structure only when told") {
    const json::Value R = requests::editorOpen("/p/main.swift", "let a = 1");
    CHECK(R["key.request"].asString() == "source.request.editor.open");
    CHECK(R["key.name"].asString() == "/p/main.swift");
    CHECK(R["key.sourcetext"].asString() == "let a = 1");
    CHECK(R["key.syntactic_only"].asInt() == 1);
    CHECK(R["key.enablesubstructure"].asInt() == 0);
    CHECK(requests::editorOpen("x", "", true)["key.enablesubstructure"].asInt() == 1);
  }

  TEST_CASE("replacetext carries a byte offset and a byte length") {
    const json::Value R = requests::editorReplaceText("/p/a.swift", ByteOffset{12}, 3, "xyz");
    CHECK(R["key.request"].asString() == "source.request.editor.replacetext");
    CHECK(R["key.offset"].asInt() == 12);
    CHECK(R["key.length"].asInt() == 3);
    CHECK(R["key.sourcetext"].asString() == "xyz");
  }

  TEST_CASE("semantic requests carry the file, the offset and every compiler argument") {
    for (const json::Value &R : {requests::cursorInfo("/p/main.swift", ByteOffset{8}, Args),
                                 requests::relatedIdents("/p/main.swift", ByteOffset{8}, Args),
                                 requests::signatureHelp("/p/main.swift", ByteOffset{8}, Args)}) {
      CHECK(R["key.sourcefile"].asString() == "/p/main.swift");
      CHECK(R["key.offset"].asInt() == 8);
      REQUIRE(R["key.compilerargs"].size() == Args.size());
      CHECK(R["key.compilerargs"][std::size_t{1}].asString() == "/p/Geometry.swift");
    }
  }

  TEST_CASE("completion sends the current text, which may not be saved") {
    const json::Value R = requests::codeComplete("/p/main.swift", "b.", ByteOffset{2}, Args);
    CHECK(R["key.request"].asString() == "source.request.codecomplete");
    CHECK(R["key.sourcetext"].asString() == "b.");
  }

  TEST_CASE("diagnostics need no offset") {
    const json::Value R = requests::diagnostics("/p/main.swift", Args);
    CHECK(R["key.request"].asString() == "source.request.diagnostics");
    CHECK_FALSE(R.has("key.offset"));
  }
}
