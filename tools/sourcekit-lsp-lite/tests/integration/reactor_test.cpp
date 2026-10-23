// The reactor entry points (reactor.cpp), the way a JavaScript host calls them,
// against the real sourcekitd: one message per call, output read back from
// the module's buffer, deferred diagnostics run by lsp_idle.

#include "json.h"
#include "text.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
void *lsp_alloc(std::size_t Size);
void lsp_free(void *Pointer);
std::size_t lsp_handle(const char *Message, std::size_t Length);
std::size_t lsp_idle();
int lsp_has_idle_work();
const char *lsp_output();
int lsp_exit_code();
}

namespace {

using lite::json::Value;

/// Sends one message through lsp_alloc'd memory, as a host must, and returns
/// the messages that came out.
std::vector<Value> call(const std::string &Message) {
  void *Buffer = lsp_alloc(Message.size());
  REQUIRE(Buffer != nullptr);
  Message.copy(static_cast<char *>(Buffer), Message.size());
  const std::size_t N = lsp_handle(static_cast<const char *>(Buffer), Message.size());
  lsp_free(Buffer);
  std::vector<Value> Out;
  std::istringstream Lines(std::string(lsp_output(), N));
  for (std::string Line; std::getline(Lines, Line);) {
    auto V = lite::json::parse(Line);
    REQUIRE(V);
    Out.push_back(std::move(*V));
  }
  return Out;
}

std::string readFile(const std::string &Path) {
  std::ifstream In(Path);
  std::stringstream S;
  S << In.rdbuf();
  return S.str();
}

std::string message(const std::string &Method, Value Params, int Id = 0) {
  Value M = Value::object();
  M.set("jsonrpc", "2.0");
  if (Id != 0)
    M.set("id", Id);
  M.set("method", Method).set("params", std::move(Params));
  return lite::json::serialize(M);
}

} // namespace

TEST_CASE("a host drives the reactor one message at a time and idles for diagnostics") {
  const std::string Root = LITE_PROJECT;
  Value Init = Value::object();
  Init.set("rootUri", lite::uriFromPath(Root)).set("capabilities", Value::object());
  Value Options = Value::object();
  Value Exclude = Value::array();
  Exclude.push("Tests/");
  Options.set("exclude", std::move(Exclude));
  Init.set("initializationOptions", std::move(Options));
  const auto Initialized = call(message("initialize", std::move(Init), 1));
  REQUIRE(Initialized.size() == 1);
  CHECK(Initialized[0]["result"]["serverInfo"]["name"].asString() == "sourcekit-lsp-lite");
  CHECK(lsp_exit_code() == -1);

  for (const char *Name : {"main.swift", "Geometry.swift"}) {
    Value TD = Value::object();
    TD.set("uri", lite::uriFromPath(Root + "/" + Name))
        .set("languageId", "swift")
        .set("version", 1)
        .set("text", readFile(Root + "/" + Name));
    Value P = Value::object();
    P.set("textDocument", std::move(TD));
    CHECK(call(message("textDocument/didOpen", std::move(P))).empty()); // nothing until idle
  }

  CHECK(lsp_has_idle_work() == 1);
  std::vector<std::string> Published;
  while (lsp_has_idle_work() != 0) {
    const std::size_t N = lsp_idle();
    const auto V = lite::json::parse(std::string(lsp_output(), N > 0 ? N - 1 : 0));
    REQUIRE(V);
    CHECK((*V)["method"].asString() == "textDocument/publishDiagnostics");
    CHECK((*V)["params"]["diagnostics"].size() == 0);
    Published.push_back((*V)["params"]["uri"].asString());
  }
  CHECK(Published.size() == 2);

  Value Pos = Value::object();
  Pos.set("line", 0).set("character", 9);
  Value TD = Value::object();
  TD.set("uri", lite::uriFromPath(Root + "/main.swift"));
  Value P = Value::object();
  P.set("textDocument", std::move(TD)).set("position", std::move(Pos));
  const auto Hover = call(message("textDocument/hover", std::move(P), 2));
  REQUIRE(Hover.size() == 1);
  CHECK(Hover[0]["result"]["contents"]["value"].asString().find("struct Point") !=
        std::string::npos);

  CHECK(call(message("shutdown", Value(), 3)).size() == 1);
  CHECK(call(message("exit", Value())).empty());
  CHECK(lsp_exit_code() == 0);
}
