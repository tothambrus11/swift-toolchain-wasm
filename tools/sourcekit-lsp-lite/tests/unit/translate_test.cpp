// Response translation, on sourcekitd responses recorded from the sample
// project (tests/fixtures, made with SOURCEKIT_LSP_LITE_RECORD).

#include "translate.h"

#include <doctest/doctest.h>

#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace lite;

namespace {

struct Fixture {
  std::string Text;
  json::Value Response;
  LineTable Lines;
};

Fixture load(const std::string &Name) {
  std::ifstream In(std::string(LITE_FIXTURES) + "/" + Name + ".json");
  std::stringstream S;
  S << In.rdbuf();
  std::string Error;
  const std::optional<json::Value> V = json::parse(S.str(), &Error);
  REQUIRE_MESSAGE(V, Name << ": " << Error);
  Fixture F{(*V)["text"].asString(), (*V)["response"], LineTable()};
  F.Lines = LineTable(F.Text);
  return F;
}

class FakeFiles final : public FileSource {
public:
  std::map<std::string, std::string, std::less<>> Texts;
  std::optional<std::string> text(const std::string &Path) override {
    const auto It = Texts.find(Path);
    return It == Texts.end() ? std::nullopt : std::optional<std::string>(It->second);
  }
  std::string uri(const std::string &Path) override { return "file://" + Path; }
};

json::Value pos(std::int64_t Line, std::int64_t Character) {
  json::Value P = json::Value::object();
  P.set("line", Line).set("character", Character);
  return P;
}

} // namespace

TEST_SUITE("translate") {
  TEST_CASE("a type error becomes one LSP error on the expression, with its id") {
    const Fixture F = load("diagnostics");
    FakeFiles Files;
    const DocumentView Doc(F.Text, F.Lines, Encoding::UTF16);
    const json::Value D = diagnostics(F.Response, "/project/main.swift", Doc, Files);
    REQUIRE(D.size() == 1);
    CHECK(D[std::size_t{0}]["severity"].asInt() == 1);
    CHECK(D[std::size_t{0}]["code"].asString() == "cannot_convert_initializer_value");
    CHECK(D[std::size_t{0}]["range"]["start"] == pos(4, 13));
    CHECK(D[std::size_t{0}]["range"]["end"] == pos(4, 21));
  }

  TEST_CASE("diagnostics sourcekitd attributes to another file are not this file's") {
    const Fixture F = load("diagnostics");
    FakeFiles Files;
    const DocumentView Doc(F.Text, F.Lines, Encoding::UTF16);
    CHECK(diagnostics(F.Response, "/project/Geometry.swift", Doc, Files).size() == 0);
  }

  TEST_CASE("a diagnostic without ranges covers the word at its column, and notes become related "
            "information") {
    const std::string Text = "let p = Pointt(x: 1)\n";
    const LineTable Lines(Text);
    const auto R = json::parse(R"({"key.diagnostics":[{"key.line":1,"key.column":9,
      "key.filepath":"/p/main.swift","key.severity":"source.diagnostic.severity.error",
      "key.description":"cannot find 'Pointt' in scope","key.diagnostics":[{"key.line":2,
      "key.column":8,"key.filepath":"/p/Geometry.swift","key.description":"did you mean 'Point'?"}]}]})");
    REQUIRE(R);
    FakeFiles Files;
    Files.Texts["/p/Geometry.swift"] = "/// A point.\nstruct Point {}\n";
    const json::Value D =
        diagnostics(*R, "/p/main.swift", DocumentView(Text, Lines, Encoding::UTF16), Files);
    REQUIRE(D.size() == 1);
    CHECK(D[std::size_t{0}]["range"]["start"] == pos(0, 8));
    CHECK(D[std::size_t{0}]["range"]["end"] == pos(0, 14));
    const json::Value &Related = D[std::size_t{0}]["relatedInformation"][std::size_t{0}];
    CHECK(Related["location"]["uri"].asString() == "file:///p/Geometry.swift");
    CHECK(Related["location"]["range"]["start"] == pos(1, 7));
  }

  TEST_CASE("member completion after b. lists Point's members, as snippets, at the cursor") {
    const Fixture F = load("codecomplete");
    const DocumentView Doc(F.Text, F.Lines, Encoding::UTF16);
    const ByteOffset Cursor{F.Text.size() - 1}; // after "b."
    const json::Value L = completionList(F.Response, Doc, {Cursor, Cursor, true, 200});
    CHECK_FALSE(L["isIncomplete"].asBool(true));
    std::map<std::string, const json::Value *> ByName;
    for (const json::Value &I : L["items"].elements())
      ByName[I["filterText"].asString()] = &I;
    REQUIRE(ByName.count("norm()") == 1);
    const json::Value &Norm = *ByName["norm()"];
    CHECK(Norm["kind"].asInt() == 2);
    CHECK(Norm["detail"].asString() == "Double");
    CHECK(Norm["textEdit"]["newText"].asString() == "norm()");
    CHECK(Norm["textEdit"]["range"]["start"] == pos(4, 2));
    CHECK(ByName.count("x") == 1);
    CHECK((*ByName["x"])["kind"].asInt() == 10);
  }

  TEST_CASE("completion filters by what is typed and caps the list") {
    const Fixture F = load("codecomplete");
    // Pretend "de" was typed after the dot.
    const std::string Text = F.Text.substr(0, F.Text.size() - 1) + "de\n";
    const LineTable Lines(Text);
    const DocumentView Doc(Text, Lines, Encoding::UTF16);
    const ByteOffset Start{F.Text.size() - 1};
    const json::Value L = completionList(F.Response, Doc, {Start, {Start.Value + 2}, false, 200});
    REQUIRE(L["items"].size() == 1);
    CHECK(L["items"][std::size_t{0}]["filterText"].asString() == "description");
    CHECK(L["items"][std::size_t{0}]["textEdit"]["range"]["start"] == pos(4, 2));
    CHECK(L["items"][std::size_t{0}]["textEdit"]["range"]["end"] == pos(4, 4));
    const json::Value Capped =
        completionList(F.Response, DocumentView(F.Text, F.Lines, Encoding::UTF16),
                       {{F.Text.size() - 1}, {F.Text.size() - 1}, false, 2});
    CHECK(Capped["items"].size() == 2);
    CHECK(Capped["isIncomplete"].asBool());
  }

  TEST_CASE("hover shows the declaration without markup, then the doc comment") {
    const Fixture F = load("cursorinfo_point");
    const DocumentView Doc(F.Text, F.Lines, Encoding::UTF16);
    const json::Value H = hover(F.Response, Doc, ByteOffset{10});
    CHECK(H["contents"]["kind"].asString() == "markdown");
    CHECK(H["contents"]["value"].asString() ==
          "```swift\nstruct Point : CustomStringConvertible\n```\n\n---\n\nA point in the plane.");
    CHECK(H["range"]["start"] == pos(0, 8));
    CHECK(H["range"]["end"] == pos(0, 13));
  }

  TEST_CASE("definition is cursorinfo's file and range, in that file's positions") {
    const Fixture F = load("cursorinfo_point");
    FakeFiles Files;
    Files.Texts["/project/Geometry.swift"] = load("substructure").Text;
    const json::Value D = definition(F.Response, Files, Encoding::UTF16);
    REQUIRE(D.size() == 1);
    CHECK(D[std::size_t{0}]["uri"].asString() == "file:///project/Geometry.swift");
    CHECK(D[std::size_t{0}]["range"]["start"] == pos(1, 7));
    CHECK(D[std::size_t{0}]["range"]["end"] == pos(1, 12));
  }

  TEST_CASE("a declaration without a file has no definition") {
    const auto R =
        json::parse(R"J({"key.name":"print(_:separator:terminator:)","key.usr":"s:s5print"})J");
    REQUIRE(R);
    FakeFiles Files;
    CHECK(definition(*R, Files, Encoding::UTF16).size() == 0);
  }

  TEST_CASE("relatedidents become highlights, the declaration marked as written") {
    const Fixture F = load("relatedidents");
    const DocumentView Doc(F.Text, F.Lines, Encoding::UTF16);
    const json::Value H = highlights(F.Response, Doc);
    REQUIRE(H.size() == 4);
    CHECK(H[std::size_t{0}]["kind"].asInt() == 3);
    CHECK(H[std::size_t{1}]["kind"].asInt() == 2);
    CHECK(identRanges(F.Response).size() == 4);
  }

  TEST_CASE("the outline has declarations only, nested, with their names selected") {
    const Fixture F = load("substructure");
    const DocumentView Doc(F.Text, F.Lines, Encoding::UTF16);
    const json::Value S = documentSymbols(F.Response, Doc);
    REQUIRE(S.size() == 2);
    const json::Value &Point = S[std::size_t{0}];
    CHECK(Point["name"].asString() == "Point");
    CHECK(Point["kind"].asInt() == 23);
    CHECK(Point["selectionRange"]["start"] == pos(1, 7));
    REQUIRE(Point["children"].size() == 4);
    CHECK(Point["children"][std::size_t{2}]["name"].asString() == "norm()");
    CHECK(Point["children"][std::size_t{2}]["kind"].asInt() == 6);
    // distance's parameters and the calls in its body are not outline entries.
    CHECK(S[std::size_t{1}]["name"].asString() == "distance(_:_:)");
    CHECK_FALSE(S[std::size_t{1}].has("children"));
  }

  TEST_CASE("signature help gives parameter label offsets in the client's units") {
    const Fixture F = load("signaturehelp");
    const json::Value H = signatureHelp(F.Response, Encoding::UTF16);
    REQUIRE(H["signatures"].size() == 1);
    CHECK(H["activeParameter"].asInt() == 1);
    const json::Value &Sig = H["signatures"][std::size_t{0}];
    CHECK(Sig["label"].asString() == "distance(_ a: Point, _ b: Point) -> Double");
    CHECK(Sig["parameters"][std::size_t{1}]["label"][std::size_t{0}].asInt() == 21);
    CHECK(Sig["parameters"][std::size_t{1}]["label"][std::size_t{1}].asInt() == 31);
  }

  TEST_CASE("signature label offsets count UTF-16 units when the label is not ASCII") {
    const auto R = json::parse(R"J({"key.signatures":[{"key.name":"f(_ é: Int, _ b: Int)",
      "key.parameters":[{"key.nameoffset":2,"key.namelength":9},{"key.nameoffset":13,"key.namelength":8}]}],
      "key.active_signature":0})J");
    REQUIRE(R);
    const json::Value H = signatureHelp(*R, Encoding::UTF16);
    const json::Value &Second =
        H["signatures"][std::size_t{0}]["parameters"][std::size_t{1}]["label"];
    CHECK(Second[std::size_t{0}].asInt() == 12); // "é" is two bytes, one unit
    CHECK(Second[std::size_t{1}].asInt() == 20);
    CHECK(signatureHelp(*json::parse(R"({"key.signatures":[]})"), Encoding::UTF16).isNull());
  }

  TEST_CASE("a cursorinfo refers to a USR as its symbol or a secondary one") {
    const Fixture F = load("cursorinfo_point");
    CHECK(refersTo(F.Response, "s:3App5PointV"));
    CHECK(refersTo(F.Response, "s:3App5PointV1x1yACSd_Sdtcfc")); // init(x:y:)
    CHECK_FALSE(refersTo(F.Response, "s:3App8distanceySdAA5PointV_ADtF"));
    CHECK_FALSE(refersTo(F.Response, ""));
  }

  TEST_CASE("renaming Point at a call renames the struct, not its synthesized initializer") {
    const Fixture F = load("cursorinfo_point");
    const RenameTarget T = renameTarget(F.Response, "Point");
    CHECK(T.Refusal.empty());
    CHECK(T.USR == "s:3App5PointV");
    CHECK(T.Name == "Point");
    CHECK(renameTarget(F.Response, "`Point`").USR == "s:3App5PointV");
  }

  TEST_CASE("the standard library, initializers, operators and empty answers cannot be renamed") {
    const auto Print = json::parse(R"J({"key.kind":"source.lang.swift.ref.function.free",
      "key.name":"print(_:separator:terminator:)","key.usr":"s:s5print","key.is_system":true,
      "key.modulename":"Swift"})J");
    REQUIRE(Print);
    CHECK(renameTarget(*Print, "print").Refusal.find("outside the project") != std::string::npos);
    const auto Init = json::parse(R"J({"key.kind":"source.lang.swift.decl.function.constructor",
      "key.name":"init(x:)","key.usr":"s:1A","key.filepath":"/p/a.swift"})J");
    REQUIRE(Init);
    CHECK_FALSE(renameTarget(*Init, "init").Refusal.empty());
    const auto Plus = json::parse(R"J({"key.kind":"source.lang.swift.decl.function.operator.infix",
      "key.name":"+(_:_:)","key.usr":"s:1B","key.filepath":"/p/a.swift"})J");
    REQUIRE(Plus);
    CHECK_FALSE(renameTarget(*Plus, "+").Refusal.empty());
    CHECK_FALSE(renameTarget(json::Value(), "x").Refusal.empty());
    const Fixture F = load("cursorinfo_point");
    CHECK_FALSE(renameTarget(F.Response, "x").Refusal.empty()); // not this symbol's name
  }

  TEST_CASE("a new name is one identifier, keywords in backticks, and never argument labels") {
    CHECK(renameNameProblem("total").empty());
    CHECK(renameNameProblem("_x2").empty());
    CHECK(renameNameProblem("café").empty());
    CHECK(renameNameProblem("`class`").empty());
    CHECK(renameNameProblem("get").empty()); // contextual keywords are names
    CHECK(renameNameProblem("class").find("keyword") != std::string::npos);
    CHECK(renameNameProblem("moved(by:)").find("argument labels") != std::string::npos);
    CHECK(renameNameProblem("moved(to:)").find("swift-syntax") != std::string::npos);
    CHECK_FALSE(renameNameProblem("").empty());
    CHECK_FALSE(renameNameProblem("_").empty());
    CHECK_FALSE(renameNameProblem("2d").empty());
    CHECK_FALSE(renameNameProblem("a b").empty());
    CHECK_FALSE(renameNameProblem("a-b").empty());
  }

  TEST_CASE("a rename edit rewrites each occurrence in document order and nothing else") {
    FakeFiles Files;
    Files.Texts["/p/a.swift"] = "let b = 1\nprint(b, `b`)\n";
    Files.Texts["/p/c.swift"] = "let 😀 = b\n";
    const std::vector<Occurrence> Where{
        {"/p/a.swift", {{16}, {17}}},  {"/p/a.swift", {{4}, {5}}},
        {"/p/a.swift", {{4}, {5}}},   // reported twice
        {"/p/a.swift", {{19}, {22}}}, // `b`
        {"/p/a.swift", {{10}, {15}}}, // "print": not b, left alone
        {"/p/c.swift", {{11}, {12}}}, // after a surrogate pair
        {"/p/gone.swift", {{0}, {1}}},
    };
    const json::Value E = renameEdit(Where, "b", "total", Files, Encoding::UTF16);
    const json::Value &A = E["changes"]["file:///p/a.swift"];
    REQUIRE(A.size() == 3);
    CHECK(A[std::size_t{0}]["range"]["start"]["character"].asInt() == 4);
    CHECK(A[std::size_t{1}]["range"]["start"]["line"].asInt() == 1);
    CHECK(A[std::size_t{1}]["range"]["start"]["character"].asInt() == 6);
    CHECK(A[std::size_t{2}]["range"]["end"]["character"].asInt() == 12);
    CHECK(A[std::size_t{2}]["newText"].asString() == "total");
    const json::Value &C = E["changes"]["file:///p/c.swift"];
    REQUIRE(C.size() == 1);
    CHECK(C[std::size_t{0}]["range"]["start"]["character"].asInt() == 9);
    CHECK_FALSE(E["changes"].has("file:///p/gone.swift"));
  }

  TEST_CASE("inlay hints name the inferred types and skip the annotated ones") {
    const Fixture F = load("variabletype");
    const DocumentView Doc(F.Text, F.Lines, Encoding::UTF16);
    const json::Value H = inlayHints(F.Response, Doc);
    REQUIRE(H.size() == 2);
    CHECK(H[std::size_t{0}]["label"].asString() == ": Point");
    CHECK(H[std::size_t{0}]["position"]["line"].asInt() == 0);
    CHECK(H[std::size_t{0}]["position"]["character"].asInt() == 5); // after "a"
    CHECK(H[std::size_t{1}]["label"].asString() == ": [Int]");
    CHECK(H[std::size_t{1}]["position"]["line"].asInt() == 2);
    CHECK(H[std::size_t{1}]["position"]["character"].asInt() == 6); // after "xs"
    CHECK(H[std::size_t{1}]["kind"].asInt() == 1);
  }

  TEST_CASE("fix-its become quick fixes on their diagnostics, and come back as code actions") {
    const Fixture F = load("diagnostics_fixits");
    FakeFiles Files;
    Files.Texts["/project/main.swift"] = F.Text;
    const DocumentView Doc(F.Text, F.Lines, Encoding::UTF16);
    const json::Value D = diagnostics(F.Response, "/project/main.swift", Doc, Files);
    REQUIRE(D.size() == 2);
    // The note's fix-it: titled by the note, placeholders as plain names.
    const json::Value &Stubs = D[std::size_t{0}]["codeActions"][std::size_t{0}];
    CHECK(Stubs["title"].asString() == "Add stubs for conformance");
    CHECK(Stubs["kind"].asString() == "quickfix");
    const json::Value &Insert =
        Stubs["edit"]["changes"]["file:///project/main.swift"][std::size_t{0}];
    CHECK(Insert["newText"].asString().find("func area() -> Double") != std::string::npos);
    CHECK(Insert["newText"].asString().find("<#") == std::string::npos);
    CHECK(Insert["range"]["start"]["line"].asInt() == 1);
    CHECK(Stubs["diagnostics"][std::size_t{0}]["message"].asString() ==
          D[std::size_t{0}]["message"].asString());
    CHECK_FALSE(Stubs["diagnostics"][std::size_t{0}].has("codeActions"));
    // The diagnostic's own fix-its: one preferred action, titled by the edit.
    const json::Value &Convert = D[std::size_t{1}]["codeActions"][std::size_t{0}];
    CHECK(Convert["title"].asString() == "Insert 'Int(', insert ')'");
    CHECK(Convert["isPreferred"].asBool());
    CHECK(Convert["edit"]["changes"]["file:///project/main.swift"].size() == 2);

    json::Value Params = json::Value::object();
    json::Value Context = json::Value::object();
    json::Value Sent = json::Value::array();
    Sent.push(D[std::size_t{0}]);
    Sent.push(D[std::size_t{1}]);
    Sent.push(D[std::size_t{1}]); // the same diagnostic twice: its fix once
    Context.set("diagnostics", Sent);
    Params.set("context", Context);
    CHECK(codeActions(Params).size() == 2);
    json::Value Only = json::Value::array();
    Only.push("refactor");
    Context.set("only", Only);
    Params.set("context", Context);
    CHECK(codeActions(Params).size() == 0);
  }

  TEST_CASE("kinds map to LSP's enumerations") {
    CHECK(completionKind("source.lang.swift.decl.function.method.instance") == 2);
    CHECK(completionKind("source.lang.swift.decl.function.free") == 3);
    CHECK(completionKind("source.lang.swift.decl.struct") == 22);
    CHECK(completionKind("source.lang.swift.keyword") == 14);
    CHECK(symbolKind("source.lang.swift.decl.extension") == 3);
    CHECK(symbolKind("source.lang.swift.decl.var.local") == 0);
    CHECK(symbolKind("source.lang.swift.expr.call") == 0);
    CHECK(severity("source.diagnostic.severity.warning") == 2);
    CHECK(severity("source.diagnostic.severity.note") == 3);
  }
}
