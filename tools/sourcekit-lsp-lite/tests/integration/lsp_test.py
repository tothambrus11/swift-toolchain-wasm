#!/usr/bin/env python3
"""LSP integration tests for sourcekit-lsp-lite.

Starts the real server (native, or the wasm module under a WASI runtime with
--server-cmd) and drives it over stdio with Content-Length framing, against the
sample project in tests/project: main.swift, Geometry.swift and
Tests/GeometryTests.swift (XCTest, `@testable import App`).

    lsp_test.py --server build/dev/sourcekit-lsp-lite --project tests/project
    lsp_test.py ... --only completion        # one scenario (what ctest runs)
    lsp_test.py ... --report                 # all scenarios, then a latency table

Each scenario is a function below; its name is what --only takes. Every wait
has a timeout, so a hung server fails the test instead of the CI job.
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import shlex
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any, Callable

# Seconds for any one answer. A cold native sourcekitd takes a few; under a
# WASI runtime, set LITE_TEST_TIMEOUT higher.
TIMEOUT = float(os.environ.get("LITE_TEST_TIMEOUT", "60"))


class Failure(Exception):
    pass


def check(condition: bool, what: str, detail: Any = "") -> None:
    if not condition:
        raise Failure(f"{what}: {detail}" if detail != "" else what)


class Client:
    """A minimal LSP client: one reader thread, requests answered in order."""

    def __init__(self, command: list[str], stderr_path: Path):
        self.stderr = open(stderr_path, "wb")
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.stderr)
        self.inbox: queue.Queue[dict | None] = queue.Queue()
        self.notifications: list[dict] = []
        self.next_id = 1
        self.timings: list[tuple[str, float]] = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        out = self.proc.stdout
        assert out is not None
        while True:
            length = None
            while True:
                line = out.readline()
                if not line:
                    self.inbox.put(None)
                    return
                line = line.strip()
                if not line:
                    break
                name, _, value = line.decode("ascii").partition(":")
                if name.lower() == "content-length":
                    length = int(value)
            if length is None:
                self.inbox.put({"protocol-error": "no Content-Length"})
                continue
            self.inbox.put(json.loads(out.read(length)))

    def send(self, message: dict) -> None:
        self.send_raw(json.dumps(message).encode())

    def send_raw(self, body: bytes) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.proc.stdin.flush()

    def _take(self, deadline: float) -> dict:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise Failure("timed out waiting for the server")
        try:
            msg = self.inbox.get(timeout=remaining)
        except queue.Empty:
            raise Failure("timed out waiting for the server") from None
        if msg is None:
            raise Failure(f"the server exited (status {self.proc.poll()})")
        return msg

    def request(self, method: str, params: Any, label: str | None = None) -> dict:
        """The whole response message, {"result": ...} or {"error": ...}."""
        rid = self.next_id
        self.next_id += 1
        start = time.monotonic()
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        deadline = start + TIMEOUT
        while True:
            msg = self._take(deadline)
            if msg.get("id") == rid and "method" not in msg:
                self.timings.append((label or method, (time.monotonic() - start) * 1000))
                return msg
            self.notifications.append(msg)

    def result(self, method: str, params: Any, label: str | None = None) -> Any:
        msg = self.request(method, params, label)
        check("error" not in msg, f"{method} failed", msg.get("error"))
        return msg.get("result")

    def notify(self, method: str, params: Any) -> None:
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def wait_notification(self, match: Callable[[dict], bool], since: int = 0,
                          label: str | None = None) -> dict:
        start = time.monotonic()
        deadline = start + TIMEOUT
        index = since
        while True:
            while index < len(self.notifications):
                msg = self.notifications[index]
                index += 1
                if match(msg):
                    if label:
                        self.timings.append((label, (time.monotonic() - start) * 1000))
                    return msg
            self.notifications.append(self._take(deadline))

    def close(self) -> int | None:
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait(timeout=10)
        self.stderr.close()
        return self.proc.returncode


class Session:
    """A server with the sample project open."""

    def __init__(self, args: argparse.Namespace, scratch: Path):
        self.project = Path(args.project).resolve()
        self.client = Client(server_command(args), scratch / "server.log")
        self.files = {
            "main.swift": (self.project / "main.swift").read_text(),
            "Geometry.swift": (self.project / "Geometry.swift").read_text(),
        }
        if not args.exclude_tests:
            self.files["Tests/GeometryTests.swift"] = (
                self.project / "Tests/GeometryTests.swift").read_text()
        self.versions = {name: 1 for name in self.files}
        init_options: dict[str, Any] = {"compilerArgs": args.compiler_arg or []}
        if args.exclude_tests:
            init_options["exclude"] = ["Tests/"]
        self.init = self.client.result("initialize", {
            "processId": os.getpid(),
            "rootUri": self.project.as_uri(),
            "capabilities": {
                "textDocument": {"completion": {"completionItem": {"snippetSupport": True}}}},
            "initializationOptions": init_options,
        })
        self.client.notify("initialized", {})

    def uri(self, name: str) -> str:
        return (self.project / name).as_uri()

    def open_all(self) -> None:
        for name, text in self.files.items():
            self.client.notify("textDocument/didOpen", {"textDocument": {
                "uri": self.uri(name), "languageId": "swift", "version": 1, "text": text}})

    def replace(self, name: str, text: str) -> int:
        """Replaces a document's whole text; returns the notification index
        after which its diagnostics will arrive."""
        since = len(self.client.notifications)
        self.files[name] = text
        self.versions[name] += 1
        self.client.notify("textDocument/didChange", {
            "textDocument": {"uri": self.uri(name), "version": self.versions[name]},
            "contentChanges": [{"text": text}]})
        return since

    def diagnostics(self, name: str, since: int = 0, label: str | None = None,
                    version: int | None = None) -> list[dict]:
        uri = self.uri(name)
        want = self.versions[name] if version is None else version
        msg = self.client.wait_notification(
            lambda m: m.get("method") == "textDocument/publishDiagnostics"
            and m["params"]["uri"] == uri and m["params"].get("version", want) == want,
            since, label)
        return msg["params"]["diagnostics"]

    def at(self, name: str, needle: str, delta: int = 0, nth: int = 0) -> dict:
        """The LSP position (UTF-16) of the nth occurrence of needle, plus delta."""
        text = self.files[name]
        index = -1
        for _ in range(nth + 1):
            index = text.index(needle, index + 1)
        before = text[:index + delta]
        line = before.count("\n")
        column = before[before.rfind("\n") + 1:]
        return {"line": line, "character": len(column.encode("utf-16-le")) // 2}

    def doc(self, name: str, position: dict) -> dict:
        return {"textDocument": {"uri": self.uri(name)}, "position": position}

    def finish(self) -> None:
        self.client.result("shutdown", None)
        self.client.notify("exit", None)
        code = self.client.proc.wait(timeout=30)
        check(code == 0, "exit after shutdown returns 0", code)


def server_command(args: argparse.Namespace) -> list[str]:
    if args.server_cmd:
        return shlex.split(args.server_cmd)
    return [args.server, *args.server_arg]


def errors(diags: list[dict]) -> list[dict]:
    return [d for d in diags if d.get("severity") == 1]


def labels(items: list[dict]) -> list[str]:
    return [i.get("filterText") or i["label"] for i in items]


def short(locations: list[dict]) -> list[str]:
    return [f"{Path(l['uri']).name}:{l['range']['start']['line'] + 1}:"
            f"{l['range']['start']['character'] + 1}" for l in locations]


# --- scenarios -----------------------------------------------------------------

def lifecycle(args, scratch):
    client = Client(server_command(args), scratch / "server.log")
    try:
        early = client.request("textDocument/hover", {})
        check(early.get("error", {}).get("code") == -32002, "a request before initialize is refused", early)
        s_init = client.result("initialize", {"processId": None, "rootUri": None, "capabilities": {}})
        caps = s_init["capabilities"]
        for provider in ["completionProvider", "hoverProvider", "definitionProvider",
                         "referencesProvider", "documentSymbolProvider", "signatureHelpProvider",
                         "renameProvider", "inlayHintProvider", "codeActionProvider"]:
            check(provider in caps, f"initialize advertises {provider}", caps)
        check(caps["positionEncoding"] == "utf-16", "UTF-16 positions unless the client offers UTF-8", caps)
        check(s_init["serverInfo"]["name"] == "sourcekit-lsp-lite", "the server says what it is", s_init)
        client.result("shutdown", None)
        late = client.request("textDocument/hover", {})
        check(late.get("error", {}).get("code") == -32600, "requests after shutdown are refused", late)
        client.notify("exit", None)
        check(client.proc.wait(timeout=30) == 0, "exit after shutdown returns 0")
    finally:
        client.close()
    client = Client(server_command(args), scratch / "server2.log")
    try:
        client.result("initialize", {"processId": None, "rootUri": None,
                                     "capabilities": {"general": {"positionEncodings": ["utf-8", "utf-16"]}}})
        client.notify("exit", None)
        check(client.proc.wait(timeout=30) == 1, "exit without shutdown returns 1")
    finally:
        client.close()


def diagnostics(args, scratch):
    s = Session(args, scratch)
    s.open_all()
    for i, name in enumerate(s.files):
        diags = s.diagnostics(name, label="diagnostics, first open (cold)" if i == 0 else None)
        check(not errors(diags), f"{name} has no errors", diags)
    since = s.replace("main.swift", s.files["main.swift"] + "let n: Int = b.norm()\n")
    diags = errors(s.diagnostics("main.swift", since, label="diagnostics after an edit (warm)"))
    check(len(diags) == 1, "an introduced type error is the one error", diags)
    d = diags[0]
    check("Double" in d["message"] and "Int" in d["message"], "it says Double is not Int", d)
    check(d["range"]["start"] == {"line": 4, "character": 13} and d["range"]["end"]["character"] == 21,
          "it covers b.norm() on line 5", d["range"])
    check(d.get("source") == "sourcekitd" and d.get("code"), "it names its source and id", d)
    since = s.replace("main.swift", s.files["main.swift"].replace("let n: Int", "let n: Double"))
    check(not errors(s.diagnostics("main.swift", since)), "fixing it clears it")
    s.finish()


def completion(args, scratch):
    s = Session(args, scratch)
    s.open_all()
    s.replace("main.swift", s.files["main.swift"] + "b.\n")
    pos = s.at("main.swift", "b.\n", 2)
    got = s.client.result("textDocument/completion", s.doc("main.swift", pos), "completion after b. (cold)")
    names = labels(got["items"])
    for member in ["x", "y", "norm()", "description"]:
        check(member in names, f"completion after b. offers {member}", names)
    norm = next(i for i in got["items"] if i["filterText"] == "norm()")
    check(norm["kind"] == 2 and norm["detail"] == "Double", "norm is a method returning Double", norm)
    check(norm["textEdit"]["range"]["start"] == pos and norm["textEdit"]["newText"] == "norm()",
          "it inserts at the cursor", norm)
    s.client.result("textDocument/completion", s.doc("main.swift", pos), "completion after b. (warm)")

    s.replace("main.swift", s.files["main.swift"].replace("b.\n", "b.no\n"))
    pos = s.at("main.swift", "b.no\n", 4)
    got = s.client.result("textDocument/completion", s.doc("main.swift", pos), "completion after b.no")
    names = labels(got["items"])
    check("norm()" in names and "x" not in names, "what is typed filters", names)
    edit = got["items"][0]["textEdit"]["range"]
    check(edit["start"]["character"] == pos["character"] - 2, "the edit replaces the typed prefix", edit)

    s.replace("main.swift", s.files["main.swift"].replace("b.no\n", "b.moved\n"))
    s.replace("Geometry.swift", s.files["Geometry.swift"].replace(
        "    var description",
        "    func moved(by dx: Double, _ dy: Double) -> Point { Point(x: x + dx, y: y + dy) }\n\n    var description"))
    pos = s.at("main.swift", "b.moved\n", 7)
    got = s.client.result("textDocument/completion", s.doc("main.swift", pos))
    moved = [i for i in got["items"] if i["filterText"].startswith("moved")]
    check(moved and moved[0]["textEdit"]["newText"] == "moved(by: ${1:Double}, ${2:dy: Double})",
          "an unsaved method from another file completes, as a snippet", moved)
    s.finish()


def hover(args, scratch):
    s = Session(args, scratch)
    s.open_all()
    got = s.client.result("textDocument/hover", s.doc("main.swift", s.at("main.swift", "Point", 2)),
                          "hover on Point (cold)")
    value = got["contents"]["value"]
    check("struct Point" in value and "CustomStringConvertible" in value, "hover shows the declaration", value)
    check("A point in the plane." in value, "hover shows the doc comment", value)
    check(got["range"]["start"] == {"line": 0, "character": 8}, "hover covers the word", got["range"])
    got = s.client.result("textDocument/hover", s.doc("main.swift", s.at("main.swift", "norm", 1)),
                          "hover on norm (warm)")
    check("func norm() -> Double" in got["contents"]["value"] and "Distance from the origin." in
          got["contents"]["value"], "hover on a method", got)
    got = s.client.result("textDocument/hover", s.doc("main.swift", s.at("main.swift", "print", 1)))
    check("func print" in got["contents"]["value"], "hover on a standard library function", got)
    got = s.client.result("textDocument/hover", s.doc("main.swift", {"line": 0, "character": 3}))
    check(got is None, "hover on a keyword has nothing to say", got)
    s.finish()


def definition(args, scratch):
    s = Session(args, scratch)
    s.open_all()
    got = s.client.result("textDocument/definition", s.doc("main.swift", s.at("main.swift", "Point", 1)),
                          "definition of Point (cross-file)")
    check(short(got) == ["Geometry.swift:2:8"], "Point is declared in Geometry.swift", short(got))
    check(got[0]["range"]["end"]["character"] == 12, "the location covers the name", got)
    got = s.client.result("textDocument/definition", s.doc("main.swift", s.at("main.swift", "distance", 1)))
    check(short(got) == ["Geometry.swift:15:6"], "distance is declared in Geometry.swift", short(got))
    got = s.client.result("textDocument/definition", s.doc("main.swift", s.at("main.swift", "b.norm", 3)))
    check(short(got) == ["Geometry.swift:7:10"], "b.norm lands on Point.norm", short(got))
    got = s.client.result("textDocument/definition", s.doc("main.swift", s.at("main.swift", "a)", 0)))
    check(short(got) == ["main.swift:1:5"], "a local lands on its declaration", short(got))
    got = s.client.result("textDocument/definition", s.doc("main.swift", s.at("main.swift", "print", 1)))
    check(got == [], "the standard library has no location, rather than a wrong one", got)
    s.finish()


def references(args, scratch):
    s = Session(args, scratch)
    s.open_all()
    pos = s.at("Geometry.swift", "struct Point", 8)
    got = s.client.result("textDocument/references", {**s.doc("Geometry.swift", pos),
                                                      "context": {"includeDeclaration": True}},
                          "references to Point (3 files)")
    where = short(got)
    files = {w.split(":")[0] for w in where}
    expected = {"Geometry.swift", "main.swift"} | (set() if args.exclude_tests else {"GeometryTests.swift"})
    check(files == expected, "references to Point span the project", where)
    check("Geometry.swift:2:8" in where and "main.swift:1:9" in where and "main.swift:2:9" in where,
          "they include the declaration and each use", where)
    without = s.client.result("textDocument/references", {**s.doc("Geometry.swift", pos),
                                                          "context": {"includeDeclaration": False}})
    check("Geometry.swift:2:8" not in short(without) and len(without) == len(got) - 1,
          "includeDeclaration: false leaves the declaration out", short(without))
    got = s.client.result("textDocument/references", {
        **s.doc("main.swift", s.at("main.swift", "norm", 1)), "context": {"includeDeclaration": True}})
    where = short(got)
    check("Geometry.swift:7:10" in where and "main.swift:4:9" in where, "references to norm", where)
    check(not any("description" in w for w in where), "only that symbol", where)
    s.finish()


def highlights(args, scratch):
    s = Session(args, scratch)
    s.open_all()
    got = s.client.result("textDocument/documentHighlight",
                          s.doc("main.swift", s.at("main.swift", "b =", 0)), "document highlight")
    check(len(got) == 4, "b is declared once and used three times", got)
    check([h["kind"] for h in got].count(3) == 1, "the declaration is marked as written", got)
    s.finish()


def apply_edits(text: str, edits: list[dict]) -> str:
    """Applies LSP TextEdits (ASCII text, so UTF-16 columns are indices)."""
    lines = text.split("\n")
    offsets = [0]
    for line in lines:
        offsets.append(offsets[-1] + len(line) + 1)
    spans = sorted(((offsets[e["range"]["start"]["line"]] + e["range"]["start"]["character"],
                     offsets[e["range"]["end"]["line"]] + e["range"]["end"]["character"],
                     e["newText"]) for e in edits), reverse=True)
    for begin, end, new in spans:
        text = text[:begin] + new + text[end:]
    return text


def rename(args, scratch):
    s = Session(args, scratch)
    s.open_all()
    s.diagnostics("main.swift")
    got = s.client.result("textDocument/prepareRename",
                          s.doc("main.swift", s.at("main.swift", "b =", 0)), "prepare rename")
    check(got["placeholder"] == "b" and got["range"]["start"] == {"line": 1, "character": 4} and
          got["range"]["end"] == {"line": 1, "character": 5}, "prepareRename names the word", got)
    refused = s.client.request("textDocument/prepareRename", s.doc("main.swift", s.at("main.swift", "print")))
    check(refused.get("error", {}).get("code") == -32803 and "outside the project" in refused["error"]["message"],
          "the standard library's print cannot be renamed, and says why", refused)
    labels_ = s.client.request("textDocument/rename", {**s.doc("main.swift", s.at("main.swift", "norm", 1)),
                                                       "newName": "length(by:)"})
    check(labels_.get("error", {}).get("code") == -32803 and "argument labels" in labels_["error"]["message"],
          "argument labels are refused rather than half-renamed", labels_)

    got = s.client.result("textDocument/rename", {**s.doc("main.swift", s.at("main.swift", "norm", 1)),
                                                  "newName": "length"}, "rename norm (3 files)")
    changes = {Path(uri).name: edits for uri, edits in got["changes"].items()}
    check(len(changes.get("Geometry.swift", [])) == 2 and len(changes.get("main.swift", [])) == 1,
          "the declaration, the use in distance and the use in main.swift", changes)
    since = len(s.client.notifications)
    for uri, edits in got["changes"].items():
        name = str(Path(uri.removeprefix("file://")).relative_to(s.project))
        s.replace(name, apply_edits(s.files[name], edits))
    check("b.length()" in s.files["main.swift"] and "func length()" in s.files["Geometry.swift"],
          "the edits rename every use", s.files)
    for name in ("main.swift", "Geometry.swift"):
        check(not errors(s.diagnostics(name, since)), f"{name} still compiles after the rename")
    got = s.client.result("textDocument/rename", {**s.doc("Geometry.swift", s.at("Geometry.swift", "Point", 1)),
                                                  "newName": "Vector"})
    total = sum(len(e) for e in got["changes"].values())
    check(total >= 6 and all(e["newText"] == "Vector" for es in got["changes"].values() for e in es),
          "a type renames at its declaration, annotations and initializer calls", got)
    s.finish()


def inlay_hints(args, scratch):
    s = Session(args, scratch)
    s.open_all()
    got = s.client.result("textDocument/inlayHint", {"textDocument": {"uri": s.uri("main.swift")},
                                                     "range": {"start": {"line": 0, "character": 0},
                                                               "end": {"line": 4, "character": 0}}},
                          "inlay hints")
    where = [(h["position"]["line"], h["position"]["character"], h["label"]) for h in got]
    check(where == [(0, 5, ": Point"), (1, 5, ": Point")], "a and b are inferred Points", where)
    s.finish()


def code_actions(args, scratch):
    s = Session(args, scratch)
    s.open_all()
    since = s.replace("main.swift", s.files["main.swift"] + "let n: Int = b.norm()\n")
    diags = errors(s.diagnostics("main.swift", since))
    check(len(diags) == 1 and diags[0].get("codeActions"), "the type error carries a fix", diags)
    got = s.client.result("textDocument/codeAction", {
        "textDocument": {"uri": s.uri("main.swift")}, "range": diags[0]["range"],
        "context": {"diagnostics": diags}}, "code actions")
    check(len(got) == 1 and got[0]["kind"] == "quickfix" and got[0]["title"].startswith("Insert 'Int('"),
          "a quick fix wraps the value in Int(...)", got)
    since = s.replace("main.swift", apply_edits(s.files["main.swift"], got[0]["edit"]["changes"][s.uri("main.swift")]))
    check("Int(b.norm())" in s.files["main.swift"] and not errors(s.diagnostics("main.swift", since)),
          "applying it fixes the error", s.files["main.swift"])
    s.finish()


def symbols(args, scratch):
    s = Session(args, scratch)
    s.open_all()
    got = s.client.result("textDocument/documentSymbol",
                          {"textDocument": {"uri": s.uri("Geometry.swift")}}, "document symbols")
    outline = [(sym["name"], sym["kind"], [c["name"] for c in sym.get("children", [])]) for sym in got]
    check(outline == [("Point", 23, ["x", "y", "norm()", "description"]), ("distance(_:_:)", 12, [])],
          "Geometry.swift's outline", outline)
    point = got[0]
    check(point["selectionRange"]["start"] == {"line": 1, "character": 7}, "the name is selected", point)
    check(point["range"]["start"]["line"] == 1 and point["range"]["end"]["line"] == 11,
          "the range is the whole declaration", point["range"])
    s.finish()


def signature_help(args, scratch):
    s = Session(args, scratch)
    s.open_all()
    s.replace("main.swift", s.files["main.swift"] + "let d = distance(a, \n")
    got = s.client.result("textDocument/signatureHelp",
                          s.doc("main.swift", s.at("main.swift", "distance(a, \n", 12)), "signature help")
    check(got and got["signatures"], "a call has a signature", got)
    sig = got["signatures"][got["activeSignature"]]
    check(sig["label"] == "distance(_ a: Point, _ b: Point) -> Double", "its label", sig)
    check(got.get("activeParameter") == 1, "the second argument is active", got)
    first, second = sig["parameters"]
    check(sig["label"][first["label"][0]:first["label"][1]] == "_ a: Point" and
          sig["label"][second["label"][0]:second["label"][1]] == "_ b: Point",
          "parameter labels are offsets into the signature", sig["parameters"])
    check("Euclidean distance" in sig.get("documentation", {}).get("value", ""), "it is documented", sig)
    s.finish()


def incremental_edits(args, scratch):
    s = Session(args, scratch)
    s.open_all()
    s.diagnostics("main.swift")
    # Type "let n: Int = b.norm()" one character at a time at the end of the
    # file, the way an editor sends it.
    end_line = s.files["main.swift"].count("\n")
    typed = "let n: Int = b.norm()\n"
    since = len(s.client.notifications)
    for i, ch in enumerate(typed):
        pos = {"line": end_line, "character": i}
        s.versions["main.swift"] += 1
        s.client.notify("textDocument/didChange", {
            "textDocument": {"uri": s.uri("main.swift"), "version": s.versions["main.swift"]},
            "contentChanges": [{"range": {"start": pos, "end": pos}, "text": ch}]})
    s.files["main.swift"] += typed
    diags = errors(s.diagnostics("main.swift", since, label="diagnostics after 22 keystrokes"))
    check(len(diags) == 1 and diags[0]["range"]["start"] == {"line": 4, "character": 13},
          "keystrokes accumulate into the same error", diags)
    # Replace "Int" with "Double" by a range edit, and delete a line by another.
    pos = s.at("main.swift", "Int")
    s.versions["main.swift"] += 1
    since = len(s.client.notifications)
    s.client.notify("textDocument/didChange", {
        "textDocument": {"uri": s.uri("main.swift"), "version": s.versions["main.swift"]},
        "contentChanges": [
            {"range": {"start": pos, "end": {"line": pos["line"], "character": pos["character"] + 3}},
             "text": "Double"},
            {"range": {"start": {"line": 3, "character": 0}, "end": {"line": 4, "character": 0}}, "text": ""},
        ]})
    s.files["main.swift"] = s.files["main.swift"].replace(": Int", ": Double").replace("print(b.norm())\n", "")
    check(not errors(s.diagnostics("main.swift", since)), "two edits in one change apply in order")
    got = s.client.result("textDocument/hover", s.doc("main.swift", s.at("main.swift", "norm", 1)))
    check(got and "func norm()" in got["contents"]["value"], "positions after edits still resolve", got)
    s.finish()


def cross_file_edits(args, scratch):
    s = Session(args, scratch)
    s.open_all()
    s.diagnostics("main.swift")
    since = s.replace("Geometry.swift", s.files["Geometry.swift"].replace("func norm()", "func length()").replace(
        "b.y).norm()", "b.y).length()"))
    diags = errors(s.diagnostics("main.swift", since, version=s.versions["main.swift"],
                                 label="diagnostics of a dependent file after an unsaved edit"))
    check(any("norm" in d["message"] for d in diags), "renaming a method elsewhere breaks its use here", diags)
    since = s.replace("Geometry.swift", s.files["Geometry.swift"].replace("func length()", "func norm()").replace(
        "b.y).length()", "b.y).norm()"))
    s.diagnostics("Geometry.swift", since)
    check(not errors(s.diagnostics("main.swift", since, version=s.versions["main.swift"])),
          "and renaming it back fixes it")
    s.finish()


def utf16(args, scratch):
    s = Session(args, scratch)
    s.open_all()
    since = s.replace("main.swift", 'let s = "\U0001F600é名"; let t = Point(x: 1, y: 2)\n' + s.files["main.swift"])
    check(not errors(s.diagnostics("main.swift", since)), "non-ASCII text compiles")
    pos = s.at("main.swift", "Point(x: 1", 2)
    check(pos["character"] == 26, "the test computes UTF-16 columns", pos)
    got = s.client.result("textDocument/hover", s.doc("main.swift", pos))
    check(got and "struct Point" in got["contents"]["value"], "hover after a surrogate pair finds Point", got)
    check(got["range"] == {"start": {"line": 0, "character": 24}, "end": {"line": 0, "character": 29}},
          "and reports its range in UTF-16", got.get("range"))
    got = s.client.result("textDocument/documentHighlight", s.doc("main.swift", s.at("main.swift", "t =")))
    check(got and got[0]["range"]["start"]["character"] == 20, "highlights too", got)
    s.finish()


def errors_scenario(args, scratch):
    s = Session(args, scratch)
    got = s.client.request("textDocument/formatting", {"textDocument": {"uri": s.uri("main.swift")}})
    check(got.get("error", {}).get("code") == -32601, "an unsupported method is MethodNotFound", got)
    s.client.send_raw(b"{not json")
    bad = s.client._take(time.monotonic() + TIMEOUT)
    check(bad.get("error", {}).get("code") == -32700 and bad.get("id") is None, "malformed JSON is a ParseError", bad)
    got = s.client.result("textDocument/hover", {"textDocument": {"uri": s.uri("Nope.swift")},
                                                  "position": {"line": 0, "character": 0}})
    check(got is None, "a document that is not open has no hover", got)
    got = s.client.result("textDocument/definition", {"textDocument": {"uri": "untitled:1"},
                                                       "position": {"line": 0, "character": 0}})
    check(got == [], "nor a definition", got)
    s.finish()


SCENARIOS: dict[str, Callable] = {
    "lifecycle": lifecycle,
    "diagnostics": diagnostics,
    "completion": completion,
    "hover": hover,
    "definition": definition,
    "references": references,
    "highlights": highlights,
    "symbols": symbols,
    "rename": rename,
    "inlay_hints": inlay_hints,
    "code_actions": code_actions,
    "signature_help": signature_help,
    "incremental_edits": incremental_edits,
    "cross_file_edits": cross_file_edits,
    "utf16": utf16,
    "errors": errors_scenario,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--server", help="the sourcekit-lsp-lite executable")
    parser.add_argument("--server-arg", action="append", default=[], help="an argument for it (repeatable)")
    parser.add_argument("--server-cmd", help="the whole command, shell-quoted, e.g. a wasmtime invocation")
    parser.add_argument("--project", required=True, help="the sample project (tests/project)")
    parser.add_argument("--compiler-arg", action="append", help="sent as initializationOptions.compilerArgs")
    parser.add_argument("--exclude-tests", action="store_true",
                        help="leave Tests/ out (no XCTest where the server runs)")
    parser.add_argument("--only", choices=sorted(SCENARIOS), action="append")
    parser.add_argument("--report", action="store_true", help="print a latency table")
    args = parser.parse_args()
    if not args.server and not args.server_cmd:
        parser.error("--server or --server-cmd is required")

    timings: list[tuple[str, float]] = []
    failed = 0
    original = Client.__init__

    def recording_init(self, *a, **kw):
        original(self, *a, **kw)
        self.timings = timings

    Client.__init__ = recording_init  # type: ignore[method-assign]
    for name in args.only or SCENARIOS:
        with tempfile.TemporaryDirectory(prefix=f"lite-{name}-") as tmp:
            start = time.monotonic()
            try:
                SCENARIOS[name](args, Path(tmp))
                print(f"PASS  {name}  ({time.monotonic() - start:.1f} s)")
            except Failure as e:
                failed += 1
                print(f"FAIL  {name}: {e}")
                log = Path(tmp) / "server.log"
                if log.exists() and log.stat().st_size:
                    print("  server stderr:\n    " + log.read_text(errors="replace")[-4000:].replace("\n", "\n    "))
    if args.report:
        print("\nlatency (ms, client-measured, request sent to answer received):")
        for label, ms in timings:
            if not label.startswith("textDocument/") and label not in ("initialize", "shutdown"):
                print(f"  {ms:8.1f}  {label}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
