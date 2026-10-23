// The sourcekitd requests the server sends, built as JSON.

#include "requests.h"

namespace lite::requests {

namespace {
json::Value request(std::string_view Name) {
  json::Value R = json::Value::object();
  R.set("key.request", Name);
  return R;
}

json::Value strings(std::span<const std::string> Args) {
  json::Value A = json::Value::array();
  for (const std::string &S : Args)
    A.push(S);
  return A;
}

json::Value offset(ByteOffset At) { return json::Value::count(At.Value); }

json::Value located(std::string_view Name, std::string_view Path, ByteOffset At,
                    std::span<const std::string> Args) {
  json::Value R = request(Name);
  R.set("key.sourcefile", Path)
      .set("key.offset", offset(At))
      .set("key.compilerargs", strings(Args));
  return R;
}
} // namespace

json::Value editorOpen(std::string_view Name, std::string_view Text, bool Substructure) {
  json::Value R = request("source.request.editor.open");
  R.set("key.name", Name)
      .set("key.sourcetext", Text)
      .set("key.enablesyntaxmap", 0)
      .set("key.enablesubstructure", Substructure ? 1 : 0)
      .set("key.syntactic_only", 1);
  return R;
}

json::Value editorReplaceText(std::string_view Name, ByteOffset At, std::size_t Length,
                              std::string_view Text) {
  json::Value R = request("source.request.editor.replacetext");
  R.set("key.name", Name)
      .set("key.offset", offset(At))
      .set("key.length", json::Value::count(Length))
      .set("key.sourcetext", Text)
      .set("key.enablesyntaxmap", 0)
      .set("key.enablesubstructure", 0)
      .set("key.syntactic_only", 1);
  return R;
}

json::Value editorClose(std::string_view Name) {
  json::Value R = request("source.request.editor.close");
  R.set("key.name", Name);
  return R;
}

json::Value diagnostics(std::string_view Path, std::span<const std::string> Args) {
  json::Value R = request("source.request.diagnostics");
  R.set("key.sourcefile", Path).set("key.compilerargs", strings(Args));
  return R;
}

json::Value codeComplete(std::string_view Path, std::string_view Text, ByteOffset At,
                         std::span<const std::string> Args) {
  json::Value R = located("source.request.codecomplete", Path, At, Args);
  R.set("key.sourcetext", Text);
  return R;
}

json::Value cursorInfo(std::string_view Path, ByteOffset At, std::span<const std::string> Args) {
  return located("source.request.cursorinfo", Path, At, Args);
}

json::Value relatedIdents(std::string_view Path, ByteOffset At, std::span<const std::string> Args) {
  return located("source.request.relatedidents", Path, At, Args);
}

json::Value signatureHelp(std::string_view Path, ByteOffset At, std::span<const std::string> Args) {
  return located("source.request.signaturehelp", Path, At, Args);
}

} // namespace lite::requests
