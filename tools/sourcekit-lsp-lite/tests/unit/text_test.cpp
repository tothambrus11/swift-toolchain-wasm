// Positions: LSP's UTF-16 (or UTF-8) columns against sourcekitd's byte
// offsets, URIs, and the text sourcekitd hands back.

#include "text.h"

#include <doctest/doctest.h>

#include <string>
#include <string_view>

using namespace lite;

namespace {
ByteOffset at(const LineTable &L, std::string_view Text, std::int64_t Line, std::int64_t Ch,
              Encoding E = Encoding::UTF16) {
  return L.offsetOf(Text, Position{Line, Ch}, E);
}
Position pos(const LineTable &L, std::string_view Text, std::size_t Offset,
             Encoding E = Encoding::UTF16) {
  return L.positionOf(Text, ByteOffset{Offset}, E);
}
} // namespace

TEST_SUITE("positions") {
  TEST_CASE("ASCII columns are byte columns") {
    const std::string_view T = "let a = 1\nlet b = 2\n";
    const LineTable L(T);
    CHECK(L.lineCount() == 3);
    CHECK(at(L, T, 1, 4).Value == 14);
    CHECK(pos(L, T, 14) == Position{1, 4});
  }

  TEST_CASE("a two-byte character is one UTF-16 unit and two UTF-8 units") {
    const std::string_view T = "let \xc3\xa9t\xc3\xa9 = 1"; // let été = 1
    const LineTable L(T);
    CHECK(at(L, T, 0, 5).Value == 6); // after "let é"
    CHECK(at(L, T, 0, 6, Encoding::UTF8).Value == 6);
    CHECK(pos(L, T, 9) == Position{0, 7});
    CHECK(pos(L, T, 9, Encoding::UTF8) == Position{0, 9});
  }

  TEST_CASE("a character outside the BMP is a surrogate pair: two UTF-16 units, four bytes") {
    const std::string_view T = "s = \"\xf0\x9f\x98\x80\".count"; // s = "😀".count
    const LineTable L(T);
    CHECK(at(L, T, 0, 5).Value == 5); // before the emoji
    CHECK(at(L, T, 0, 7).Value == 9); // after it
    CHECK(at(L, T, 0, 6).Value == 5); // between its halves: rounds down
    CHECK(pos(L, T, 9) == Position{0, 7});
    CHECK(pos(L, T, 7) == Position{0, 5}); // inside the sequence: not begun
  }

  TEST_CASE("a CJK character is three bytes and one UTF-16 unit") {
    const std::string_view T = "let \xe5\x90\x8d = 1"; // let 名 = 1
    const LineTable L(T);
    CHECK(at(L, T, 0, 5).Value == 7);
    CHECK(pos(L, T, 7) == Position{0, 5});
  }

  TEST_CASE("CRLF, CR and LF all end lines, and never count as columns") {
    const std::string_view T = "a\r\nbc\rd\n";
    const LineTable L(T);
    CHECK(L.lineCount() == 4);
    CHECK(at(L, T, 1, 0).Value == 3);
    CHECK(at(L, T, 2, 0).Value == 6);
    CHECK(at(L, T, 0, 99).Value == 1);     // clamps before "\r\n"
    CHECK(pos(L, T, 2) == Position{0, 1}); // inside "\r\n"
  }

  TEST_CASE("positions past the end clamp, they do not fail") {
    const std::string_view T = "ab\ncd";
    const LineTable L(T);
    CHECK(at(L, T, 7, 0).Value == 5);
    CHECK(at(L, T, -1, 0).Value == 0);
    CHECK(pos(L, T, 99) == Position{1, 2});
  }

  TEST_CASE("sourcekitd's 1-based line and byte column map to an offset") {
    const std::string_view T = "let \xc3\xa9 = 1\nlet n: Int = x\n";
    const LineTable L(T);
    CHECK(L.offsetOf(T, LineColumn{2, 8}).Value == 18);  // 11 + 7: "é" is two bytes
    CHECK(L.offsetOf(T, LineColumn{1, 99}).Value == 10); // clamps to the line end
  }

  TEST_CASE("utf16Length counts surrogate pairs twice") {
    CHECK(utf16Length("abc") == 3);
    CHECK(utf16Length("\xc3\xa9\xf0\x9f\x98\x80") == 3);
  }
}

TEST_SUITE("words") {
  TEST_CASE("the word around a position is found from either side") {
    const std::string_view T = "let p = Point(x: 1)";
    CHECK(identifierStart(T, ByteOffset{11}).Value == 8);
    CHECK(identifierEnd(T, ByteOffset{11}).Value == 13);
    CHECK(identifierStart(T, ByteOffset{13}).Value == 8); // at its end
  }

  TEST_CASE("whole-word occurrences only") {
    const auto Found = findIdentifier("Point Points aPoint Point(", "Point");
    REQUIRE(Found.size() == 2);
    CHECK(Found[0].Value == 0);
    CHECK(Found[1].Value == 20);
  }

  TEST_CASE("base names drop argument labels") {
    CHECK(baseName("moved(by:_:)") == "moved");
    CHECK(baseName("x") == "x");
  }

  TEST_CASE("completion matching ranks prefixes above subsequences") {
    CHECK(matchRank("norm", "") == 0);
    CHECK(matchRank("norm", "no") == 0);
    CHECK(matchRank("norm", "NO") == 1);
    CHECK(matchRank("description", "dsc") == 2);
    CHECK(matchRank("description", "sc") == -1); // first letters must agree
    CHECK(matchRank("x", "xy") == -1);
  }
}

TEST_SUITE("uris") {
  TEST_CASE("file URIs decode percent escapes") {
    CHECK(pathFromURI("file:///work/My%20App/main.swift") == "/work/My App/main.swift");
    CHECK(pathFromURI("file://localhost/a.swift") == "/a.swift");
    CHECK(pathFromURI("file:///a.swift#frag") == "/a.swift");
    CHECK(pathFromURI("untitled:Untitled-1").empty());
    CHECK(pathFromURI("file://otherhost/a.swift").empty());
  }

  TEST_CASE("paths encode to URIs that decode back") {
    const std::string P = "/work/My App/\xc3\xa9#1.swift";
    CHECK(uriFromPath(P) == "file:///work/My%20App/%C3%A9%231.swift");
    CHECK(pathFromURI(uriFromPath(P)) == P);
  }
}

TEST_SUITE("markup") {
  TEST_CASE("annotated declarations lose their tags and decode entities") {
    CHECK(stripMarkup("<Declaration>func f&lt;T&gt;(_ x: <Type usr=\"s:Sd\">Double</Type>) "
                      "-&gt; &quot;A&quot; &amp; B</Declaration>") ==
          "func f<T>(_ x: Double) -> \"A\" & B");
  }

  TEST_CASE("documentation XML yields an element's text") {
    const std::string XML = "<Class><CommentParts><Abstract><Para>A point in the plane.</Para>"
                            "</Abstract></CommentParts></Class>";
    CHECK(elementText(XML, "Abstract") == "A point in the plane.");
    CHECK(elementText(XML, "Discussion").empty());
  }

  TEST_CASE("placeholders become snippet tab stops, or are cut for plain text") {
    const std::string_view S = "moved(by: <#T##Double#>, <#T##y: Double##Double#>)";
    CHECK(snippetFromPlaceholders(S) == "moved(by: ${1:Double}, ${2:y: Double})");
    CHECK(plainFromPlaceholders(S) == "moved(by: ");
    CHECK(snippetFromPlaceholders("a$b}c") == "a\\$b\\}c");
    CHECK(snippetFromPlaceholders("norm()") == "norm()");
    CHECK(snippetFromPlaceholders("f(<#x#>") == "f(${1:x}");
    CHECK(snippetFromPlaceholders("f(<#unterminated") == "f(<#unterminated");
  }
}
