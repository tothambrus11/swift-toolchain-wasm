// sourcekitd responses, as JSON, turned into LSP results.

#include "translate.h"

#include <algorithm>
#include <utility>

namespace lite {

namespace {

constexpr std::string_view DeclPrefix = "source.lang.swift.decl.";

// LSP enumerations, by the names the specification gives them.
namespace completion_kind {
constexpr int Text = 1, Method = 2, Function = 3, Constructor = 4, Variable = 6, Class = 7,
              Interface = 8, Module = 9, Property = 10, Value = 12, Enum = 13, Keyword = 14,
              EnumMember = 20, Struct = 22, Operator = 24, TypeParameter = 25;
} // namespace completion_kind
namespace symbol_kind {
constexpr int Namespace = 3, Class = 5, Method = 6, Property = 7, Constructor = 9, Enum = 10,
              Interface = 11, Function = 12, Variable = 13, EnumMember = 22, Struct = 23,
              Operator = 25, TypeParameter = 26;
} // namespace symbol_kind
constexpr int SeverityError = 1, SeverityWarning = 2, SeverityInformation = 3, SeverityHint = 4;
constexpr int HighlightRead = 2, HighlightWrite = 3;
constexpr int InsertPlainText = 1, InsertSnippet = 2;
constexpr int TagDeprecated = 1;

ByteOffset offsetIn(const json::Value &V, const char *Key) { return {V[Key].asSize()}; }

ByteRange rangeIn(const json::Value &V, const char *OffsetKey = "key.offset",
                  const char *LengthKey = "key.length") {
  const ByteOffset B = offsetIn(V, OffsetKey);
  return {B, {B.Value + V[LengthKey].asSize()}};
}

/// Clamps a range to a text of \p Size bytes, with End never before Begin.
ByteRange clamp(ByteRange R, std::size_t Size) noexcept {
  const std::size_t B = std::min(R.Begin.Value, Size);
  return {{B}, {std::clamp(R.End.Value, B, Size)}};
}

/// How close a completion's declaration is to the cursor; lower is closer.
int contextRank(std::string_view Context) noexcept {
  constexpr std::string_view Prefix = "source.codecompletion.context.";
  if (!Context.starts_with(Prefix))
    return 5;
  const std::string_view C = Context.substr(Prefix.size());
  if (C == "local")
    return 0;
  if (C == "exprspecific" || C == "thisclass")
    return 1;
  if (C == "superclass")
    return 2;
  if (C == "thismodule")
    return 3;
  if (C == "othermodule")
    return 4;
  return 5;
}

/// Zero-padded, so that sortText compares as the numbers do.
std::string padded(std::size_t N, std::size_t Width) {
  std::string S = std::to_string(N);
  if (S.size() < Width)
    S.insert(0, Width - S.size(), '0');
  return S;
}

json::Value markdown(std::string Text) {
  json::Value M = json::Value::object();
  M.set("kind", "markdown").set("value", std::move(Text));
  return M;
}

void appendSymbols(const json::Value &Structure, const DocumentView &Doc, json::Value &Out) {
  for (const json::Value &S : Structure.elements()) {
    const std::string &KindName = S["key.kind"].asString();
    const json::Value &Children = S["key.substructure"];
    if (KindName == "source.lang.swift.decl.enumcase") {
      // "case a, b" holds enum elements; the outline shows the elements.
      appendSymbols(Children, Doc, Out);
      continue;
    }
    const int Kind = symbolKind(KindName);
    if (Kind == 0) {
      if (KindName == "source.lang.swift.stmt.if_config")
        appendSymbols(Children, Doc, Out); // declarations inside #if
      continue;
    }
    const ByteRange Whole = clamp(rangeIn(S), Doc.text().size());
    ByteRange Name = S.has("key.nameoffset")
                         ? clamp(rangeIn(S, "key.nameoffset", "key.namelength"), Doc.text().size())
                         : ByteRange{Whole.Begin, Whole.Begin};
    if (Name.Begin < Whole.Begin || Name.End > Whole.End)
      Name = {Whole.Begin, Whole.Begin}; // LSP requires it inside the range
    json::Value Sym = json::Value::object();
    Sym.set("name", S["key.name"].asString())
        .set("kind", Kind)
        .set("range", Doc.range(Whole))
        .set("selectionRange", Doc.range(Name));
    if (!S["key.typename"].asString().empty())
      Sym.set("detail", S["key.typename"].asString());
    json::Value Kids = json::Value::array();
    appendSymbols(Children, Doc, Kids);
    if (Kids.size() > 0)
      Sym.set("children", std::move(Kids));
    Out.push(std::move(Sym));
  }
}

} // namespace

ByteOffset DocumentView::offsetOf(const json::Value &LSPPosition) const noexcept {
  return Lines->offsetOf(
      Text, Position{LSPPosition["line"].asInt(), LSPPosition["character"].asInt()}, Enc);
}

ByteOffset DocumentView::offsetOf(LineColumn At) const noexcept {
  return Lines->offsetOf(Text, At);
}

json::Value DocumentView::position(ByteOffset At) const {
  const Position P = Lines->positionOf(Text, At, Enc);
  json::Value V = json::Value::object();
  V.set("line", P.Line).set("character", P.Character);
  return V;
}

json::Value DocumentView::range(ByteRange R) const {
  json::Value V = json::Value::object();
  V.set("start", position(R.Begin)).set("end", position(R.End));
  return V;
}

json::Value location(FileSource &Files, Encoding Enc, const std::string &Path, ByteRange R) {
  const std::string Text = Files.text(Path).value_or(std::string());
  const LineTable Lines(Text);
  const DocumentView Doc(Text, Lines, Enc);
  json::Value L = json::Value::object();
  L.set("uri", Files.uri(Path)).set("range", Doc.range(clamp(R, Text.size())));
  return L;
}

json::Value diagnostics(const json::Value &Response, const std::string &Path,
                        const DocumentView &Doc, FileSource &Files) {
  json::Value List = json::Value::array();
  for (const json::Value &D : Response["key.diagnostics"].elements()) {
    const std::string &File = D["key.filepath"].asString();
    if (!File.empty() && File != Path)
      continue;
    ByteRange R;
    const json::Value &Ranges = D["key.ranges"];
    if (Ranges.size() > 0) {
      R = rangeIn(Ranges[std::size_t{0}]);
    } else {
      // A point: widen it to the word it is on, so it can be seen.
      R.Begin = Doc.offsetOf(LineColumn{D["key.line"].asInt(1), D["key.column"].asInt(1)});
      R.End = identifierEnd(Doc.text(), R.Begin);
    }
    json::Value Diag = json::Value::object();
    Diag.set("range", Doc.range(clamp(R, Doc.text().size())))
        .set("severity", severity(D["key.severity"].asString()))
        .set("source", "sourcekitd")
        .set("message", D["key.description"].asString());
    if (!D["key.id"].asString().empty())
      Diag.set("code", D["key.id"].asString());
    json::Value Related = json::Value::array();
    for (const json::Value &Note : D["key.diagnostics"].elements()) {
      const std::string &NoteFile = Note["key.filepath"].asString();
      if (NoteFile.empty())
        continue;
      const std::string Text = Files.text(NoteFile).value_or(std::string());
      const LineTable Lines(Text);
      const ByteOffset At =
          Lines.offsetOf(Text, LineColumn{Note["key.line"].asInt(1), Note["key.column"].asInt(1)});
      json::Value Info = json::Value::object();
      Info.set("location", location(Files, Doc.encoding(), NoteFile, {At, At}))
          .set("message", Note["key.description"].asString());
      Related.push(std::move(Info));
    }
    if (Related.size() > 0)
      Diag.set("relatedInformation", std::move(Related));
    List.push(std::move(Diag));
  }
  return List;
}

json::Value completionList(const json::Value &Response, const DocumentView &Doc,
                           const CompletionRequest &Request) {
  const std::string_view Typed =
      Doc.text().substr(Request.WordStart.Value, Request.Cursor.Value - Request.WordStart.Value);
  struct Candidate {
    std::string SortKey;
    json::Value Item;
  };
  std::vector<Candidate> Matches;
  const std::vector<json::Value> &Results = Response["key.results"].elements();
  for (std::size_t I = 0; I < Results.size(); ++I) {
    const json::Value &C = Results[I];
    const std::string &Name = C["key.name"].asString();
    const std::string_view Base = baseName(Name);
    const int Rank = matchRank(Base.empty() ? std::string_view(Name) : Base, Typed);
    if (Rank < 0)
      continue;
    // Some completions replace text before the word, e.g. the "." of an
    // optional chain.
    const std::size_t Erase = C["key.num_bytes_to_erase"].asSize();
    const ByteOffset EditBegin{Request.WordStart.Value - std::min(Erase, Request.WordStart.Value)};
    const std::string &Source = C["key.sourcetext"].asString();
    json::Value Edit = json::Value::object();
    Edit.set("range", Doc.range({EditBegin, Request.Cursor}))
        .set("newText",
             Request.Snippets ? snippetFromPlaceholders(Source) : plainFromPlaceholders(Source));
    const std::string &Description = C["key.description"].asString();
    json::Value Item = json::Value::object();
    Item.set("label", Description.empty() ? Name : Description)
        .set("kind", completionKind(C["key.kind"].asString()))
        .set("filterText", Name)
        .set("textEdit", std::move(Edit))
        .set("insertTextFormat", Request.Snippets ? InsertSnippet : InsertPlainText);
    if (!C["key.typename"].asString().empty())
      Item.set("detail", C["key.typename"].asString());
    if (!C["key.doc.brief"].asString().empty())
      Item.set("documentation", markdown(C["key.doc.brief"].asString()));
    const bool NotRecommended = C["key.not_recommended"].asInt() != 0;
    if (NotRecommended) {
      json::Value Tags = json::Value::array();
      Tags.push(TagDeprecated);
      Item.set("tags", std::move(Tags));
    }
    // Better match, then nearer declaration, then recommended, then
    // sourcekitd's own order.
    std::string Key = std::to_string(Rank) +
                      std::to_string(contextRank(C["key.context"].asString())) +
                      (NotRecommended ? "1" : "0") + padded(I, 6);
    Item.set("sortText", Key);
    Matches.push_back(Candidate{std::move(Key), std::move(Item)});
  }
  std::sort(Matches.begin(), Matches.end(),
            [](const Candidate &A, const Candidate &B) { return A.SortKey < B.SortKey; });
  const bool Truncated = Matches.size() > Request.MaxItems;
  if (Truncated)
    Matches.resize(Request.MaxItems);
  json::Value Items = json::Value::array();
  for (Candidate &C : Matches)
    Items.push(std::move(C.Item));
  json::Value List = json::Value::object();
  List.set("isIncomplete", Truncated).set("items", std::move(Items));
  return List;
}

json::Value hover(const json::Value &Response, const DocumentView &Doc, ByteOffset At) {
  const std::string &Name = Response["key.name"].asString();
  if (Name.empty())
    return {};
  std::string Decl = stripMarkup(Response["key.annotated_decl"].asString());
  if (Decl.empty())
    Decl = Name;
  std::string Comment = Response["key.doc_comment"].asString();
  if (Comment.empty()) {
    // Without key.doc_comment, the comment is only in the documentation XML.
    const std::string &XML = Response["key.doc.full_as_xml"].asString();
    const std::string Abstract = elementText(XML, "Abstract");
    const std::string Discussion = elementText(XML, "Discussion");
    Comment = Discussion.empty() ? Abstract : Abstract + "\n\n" + Discussion;
  }
  std::string Text = "```swift\n" + Decl + "\n```";
  if (!Comment.empty())
    Text += "\n\n---\n\n" + Comment;
  json::Value Result = json::Value::object();
  Result.set("contents", markdown(std::move(Text)));
  const ByteRange Word{identifierStart(Doc.text(), At), identifierEnd(Doc.text(), At)};
  if (Word.End > Word.Begin)
    Result.set("range", Doc.range(Word));
  return Result;
}

json::Value definition(const json::Value &Response, FileSource &Files, Encoding Enc) {
  json::Value Locations = json::Value::array();
  const std::string &File = Response["key.filepath"].asString();
  // A declaration in a module (the standard library) has no file; showing it
  // would need a generated interface (editor.open.interface).
  if (!File.empty() && Response.has("key.offset"))
    Locations.push(location(Files, Enc, File, rangeIn(Response)));
  return Locations;
}

std::vector<ByteRange> identRanges(const json::Value &Response) {
  std::vector<ByteRange> Ranges;
  for (const json::Value &R : Response["key.results"].elements())
    Ranges.push_back(rangeIn(R));
  return Ranges;
}

json::Value highlights(const json::Value &Response, const DocumentView &Doc) {
  json::Value Out = json::Value::array();
  for (const json::Value &R : Response["key.results"].elements()) {
    const bool Definition = R["key.nametype"].asString().ends_with(".definition");
    json::Value H = json::Value::object();
    H.set("range", Doc.range(clamp(rangeIn(R), Doc.text().size())))
        .set("kind", Definition ? HighlightWrite : HighlightRead);
    Out.push(std::move(H));
  }
  return Out;
}

json::Value documentSymbols(const json::Value &Response, const DocumentView &Doc) {
  json::Value Out = json::Value::array();
  appendSymbols(Response["key.substructure"], Doc, Out);
  return Out;
}

json::Value signatureHelp(const json::Value &Response, Encoding Enc) {
  const json::Value &Sigs = Response["key.signatures"];
  if (Sigs.size() == 0)
    return {};
  json::Value Signatures = json::Value::array();
  for (const json::Value &S : Sigs.elements()) {
    const std::string_view Label = S["key.name"].asString();
    json::Value Parameters = json::Value::array();
    for (const json::Value &P : S["key.parameters"].elements()) {
      const std::size_t B = std::min(P["key.nameoffset"].asSize(), Label.size());
      const std::size_t E = std::min(B + P["key.namelength"].asSize(), Label.size());
      // [start, end) within the label, in the client's units.
      json::Value LabelRange = json::Value::array();
      if (Enc == Encoding::UTF16) {
        LabelRange.push(json::Value::count(utf16Length(Label.substr(0, B))));
        LabelRange.push(json::Value::count(utf16Length(Label.substr(0, E))));
      } else {
        LabelRange.push(json::Value::count(B));
        LabelRange.push(json::Value::count(E));
      }
      json::Value Param = json::Value::object();
      Param.set("label", std::move(LabelRange));
      if (!P["key.doc_comment"].asString().empty())
        Param.set("documentation", markdown(P["key.doc_comment"].asString()));
      Parameters.push(std::move(Param));
    }
    json::Value Sig = json::Value::object();
    Sig.set("label", Label).set("parameters", std::move(Parameters));
    if (!S["key.doc_comment"].asString().empty())
      Sig.set("documentation", markdown(S["key.doc_comment"].asString()));
    if (S.has("key.active_parameter"))
      Sig.set("activeParameter", S["key.active_parameter"].asInt());
    Signatures.push(std::move(Sig));
  }
  const std::size_t Active = Response["key.active_signature"].asSize();
  json::Value Help = json::Value::object();
  Help.set("signatures", std::move(Signatures)).set("activeSignature", json::Value::count(Active));
  const json::Value &ActiveParameter = Sigs[Active]["key.active_parameter"];
  if (ActiveParameter.isNumber())
    Help.set("activeParameter", ActiveParameter.asInt());
  return Help;
}

bool refersTo(const json::Value &CursorInfo, std::string_view USR) {
  if (USR.empty())
    return false;
  if (CursorInfo["key.usr"].asString() == USR)
    return true;
  const auto &Secondary = CursorInfo["key.secondary_symbols"].elements();
  return std::any_of(Secondary.begin(), Secondary.end(),
                     [&](const json::Value &S) { return S["key.usr"].asString() == USR; });
}

int completionKind(std::string_view Kind) noexcept {
  namespace K = completion_kind;
  if (Kind.starts_with("source.lang.swift.keyword"))
    return K::Keyword;
  if (Kind.starts_with("source.lang.swift.literal"))
    return K::Value;
  if (Kind.starts_with("source.lang.swift.import.module") ||
      Kind == "source.lang.swift.decl.module")
    return K::Module;
  if (!Kind.starts_with(DeclPrefix))
    return K::Text;
  const std::string_view D = Kind.substr(DeclPrefix.size());
  if (D.starts_with("function.method") || D.starts_with("function.subscript"))
    return K::Method;
  if (D.starts_with("function.constructor"))
    return K::Constructor;
  if (D.starts_with("function.operator"))
    return K::Operator;
  if (D.starts_with("function"))
    return K::Function;
  if (D == "var.instance" || D == "var.static" || D == "var.class")
    return K::Property;
  if (D.starts_with("var"))
    return K::Variable;
  if (D == "struct")
    return K::Struct;
  if (D == "class" || D == "actor")
    return K::Class;
  if (D == "enum")
    return K::Enum;
  if (D == "enumelement")
    return K::EnumMember;
  if (D == "protocol")
    return K::Interface;
  if (D == "typealias" || D == "associatedtype" || D == "generic_type_param")
    return K::TypeParameter;
  return K::Text;
}

int symbolKind(std::string_view Kind) noexcept {
  namespace K = symbol_kind;
  if (!Kind.starts_with(DeclPrefix))
    return 0;
  const std::string_view D = Kind.substr(DeclPrefix.size());
  if (D == "class" || D == "actor")
    return K::Class;
  if (D == "struct")
    return K::Struct;
  if (D == "enum")
    return K::Enum;
  if (D == "protocol")
    return K::Interface;
  if (D.starts_with("extension"))
    return K::Namespace; // as SourceKit-LSP shows extensions
  if (D == "function.free")
    return K::Function;
  if (D.starts_with("function.method") || D.starts_with("function.subscript"))
    return K::Method;
  if (D.starts_with("function.constructor") || D.starts_with("function.destructor"))
    return K::Constructor;
  if (D.starts_with("function.operator"))
    return K::Operator;
  if (D == "var.instance" || D == "var.static" || D == "var.class")
    return K::Property;
  if (D == "var.global")
    return K::Variable;
  if (D == "enumelement")
    return K::EnumMember;
  if (D == "typealias" || D == "associatedtype")
    return K::TypeParameter;
  return 0;
}

int severity(std::string_view Severity) noexcept {
  if (Severity.ends_with(".error"))
    return SeverityError;
  if (Severity.ends_with(".warning"))
    return SeverityWarning;
  if (Severity.ends_with(".remark"))
    return SeverityHint;
  return SeverityInformation; // notes
}

} // namespace lite
