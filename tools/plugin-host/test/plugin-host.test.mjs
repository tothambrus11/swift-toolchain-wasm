// Unit tests for swift-plugin-host.mjs, against a C stand-in for a Swift macro
// plugin (fixtures/echo-plugin.c) that speaks the same guest ABI. The real
// plugins, with the real frontend, are tested by scripts/57-macros-smoke-test.sh.
//
//   node --test tools/plugin-host/test/
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import {
  SwiftPluginHost,
  frameMessage,
  unframeMessages,
} from "../swift-plugin-host.mjs";

const echoPlugin = new WebAssembly.Module(
  readFileSync(new URL("./fixtures/echo-plugin.wasm", import.meta.url)),
);

const capability = JSON.stringify({ getCapability: { capability: { protocolVersion: 7 } } });

function expansion(moduleName, extra = "") {
  return JSON.stringify({
    expandFreestandingMacro: {
      macro: { moduleName, typeName: "EchoMacro", name: "echo" },
      macroRole: "expression",
      discriminator: `$s4main${extra}`,
      syntax: { kind: "expression", source: "#echo", location: {} },
    },
  });
}

function newHost(overrides = {}) {
  const loads = [];
  const stderr = [];
  const host = new SwiftPluginHost({
    loadPlugin(libraryPath, moduleName) {
      loads.push([libraryPath, moduleName]);
      if (libraryPath.endsWith("missing.wasm")) throw new Error("no such file");
      return echoPlugin;
    },
    onStderr: (name, text) => stderr.push([name, text]),
    ...overrides,
  });
  return { host, loads, stderr };
}

function load(host, moduleName = "EchoMacros") {
  return host.handleMessage(
    JSON.stringify({
      loadPluginLibrary: { libraryPath: `/plugins/lib${moduleName}.wasm`, moduleName },
    }),
  );
}

test("the host answers the compiler's capability request itself, offering library loading", () => {
  const { host, loads } = newHost();
  const { ok, response } = host.handleMessage(capability);
  assert.equal(ok, true);
  assert.deepEqual(JSON.parse(response), {
    getCapabilityResult: {
      capability: { protocolVersion: 7, features: ["load-plugin-library"] },
    },
  });
  assert.deepEqual(loads, [], "no plugin is loaded before the compiler asks for one");
});

test("a macro expands in the plugin that the compiler loaded for its module", () => {
  const { host, loads } = newHost();
  host.handleMessage(capability);
  assert.deepEqual(JSON.parse(load(host).response), {
    loadPluginLibraryResult: { loaded: true, diagnostics: [] },
  });
  assert.deepEqual(loads, [["/plugins/libEchoMacros.wasm", "EchoMacros"]]);

  const { ok, response } = host.handleMessage(expansion("EchoMacros"));
  assert.equal(ok, true);
  // Request 1 was the replayed capability request, so this is request 2: the
  // plugin learned the compiler's protocol version before its first macro.
  assert.equal(
    JSON.parse(response).expandMacroResult.expandedSource,
    '"expanded by echo-plugin, request 2"',
  );
});

test("the plugin keeps its state between expansions", () => {
  const { host } = newHost();
  load(host);
  host.handleMessage(expansion("EchoMacros"));
  const { response } = host.handleMessage(expansion("EchoMacros"));
  assert.match(JSON.parse(response).expandMacroResult.expandedSource, /request 2/);
});

test("one plugin module providing several macro modules is instantiated once", () => {
  const { host, loads } = newHost();
  for (const moduleName of ["FirstMacros", "SecondMacros"]) {
    const { response } = host.handleMessage(
      JSON.stringify({ loadPluginLibrary: { libraryPath: "/plugins/Bundle.wasm", moduleName } }),
    );
    assert.equal(JSON.parse(response).loadPluginLibraryResult.loaded, true);
  }
  assert.equal(loads.length, 1);
  host.handleMessage(expansion("FirstMacros"));
  const { response } = host.handleMessage(expansion("SecondMacros"));
  assert.match(JSON.parse(response).expandMacroResult.expandedSource, /request 2/);
});

test("a plugin that cannot be loaded is reported as not loaded, with the reason", () => {
  const { host } = newHost();
  const { ok, response } = host.handleMessage(
    JSON.stringify({ loadPluginLibrary: { libraryPath: "/plugins/missing.wasm", moduleName: "M" } }),
  );
  assert.equal(ok, true, "a failed load is a reply the compiler diagnoses, not a broken channel");
  const result = JSON.parse(response).loadPluginLibraryResult;
  assert.equal(result.loaded, false);
  assert.match(result.diagnostics[0].message, /missing\.wasm.*no such file/);
});

test("a module that is not a macro plugin is refused at load", () => {
  const notAPlugin = new WebAssembly.Module(
    // (module (memory (export "memory") 1))
    new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 5, 3, 1, 0, 1, 7, 10, 1, 6, 109, 101, 109, 111, 114, 121, 2, 0]),
  );
  const { host } = newHost({ loadPlugin: () => notAPlugin });
  const result = JSON.parse(load(host).response).loadPluginLibraryResult;
  assert.equal(result.loaded, false);
  assert.match(result.diagnostics[0].message, /swift_wasm_macro_v1_pump/);
});

test("a macro from a module no plugin was loaded for fails instead of guessing", () => {
  const { host } = newHost();
  load(host, "EchoMacros");
  const { ok, response } = host.handleMessage(expansion("OtherMacros"));
  assert.equal(ok, false);
  assert.match(response, /no plugin loaded for macro module 'OtherMacros'/);
});

test("a plugin that traps fails that expansion and is restarted for the next", () => {
  const { host } = newHost();
  load(host);
  const trapped = host.handleMessage(expansion("EchoMacros", "trapMe"));
  assert.equal(trapped.ok, false);
  assert.match(trapped.response, /macro plugin 'libEchoMacros.wasm' failed on a 'EchoMacros' macro/);

  const next = host.handleMessage(expansion("EchoMacros"));
  assert.equal(next.ok, true);
  assert.match(JSON.parse(next.response).expandMacroResult.expandedSource, /request 1/);
});

test("what a plugin prints to stderr reaches the embedder, tagged with the plugin", () => {
  const { host, stderr } = newHost();
  load(host);
  host.handleMessage(expansion("EchoMacros"));
  assert.ok(stderr.every(([name]) => name === "libEchoMacros.wasm"));
  // wasi-libc's stderr is unbuffered, so the text may arrive in pieces.
  assert.equal(stderr.map(([, text]) => text).join(""), "echo-plugin: request 1\n");
});

test("unparseable requests fail without touching any plugin", () => {
  const { host } = newHost();
  const { ok, response } = host.handleMessage("{not json");
  assert.equal(ok, false);
  assert.match(response, /JSON/);
});

test("the frontend's imports read requests from and write responses to its memory", () => {
  const { host } = newHost();
  const memory = new WebAssembly.Memory({ initial: 1 });
  host.attach(memory);
  const { handle_message: handleMessage, take_response: takeResponse } =
    host.imports.swift_plugin_host;

  const request = new TextEncoder().encode(capability);
  new Uint8Array(memory.buffer).set(request, 1024);
  const status = handleMessage(1024, request.length, 16);
  assert.equal(status, 0);
  const length = new DataView(memory.buffer).getUint32(16, true);
  takeResponse(4096);
  const response = new TextDecoder().decode(new Uint8Array(memory.buffer, 4096, length));
  assert.equal(JSON.parse(response).getCapabilityResult.capability.features[0], "load-plugin-library");
  assert.equal(host.pending, null, "a taken response is forgotten");

  const bad = new TextEncoder().encode("{}");
  new Uint8Array(memory.buffer).set(bad, 1024);
  assert.equal(handleMessage(1024, bad.length, 16), 1, "errors return nonzero");
});

test("the imports refuse to run before the frontend's memory is attached", () => {
  const { host } = newHost();
  assert.throws(() => host.imports.swift_plugin_host.handle_message(0, 0, 0), /attach/);
});

test("messages are framed with a little-endian 64-bit length", () => {
  const framed = frameMessage("héllo");
  assert.deepEqual([...framed.subarray(0, 8)], [6, 0, 0, 0, 0, 0, 0, 0]);
  const two = new Uint8Array([...framed, ...frameMessage("{}")]);
  assert.deepEqual(unframeMessages(two), ["héllo", "{}"]);
  assert.throws(() => unframeMessages(framed.subarray(0, 10)), /truncated/);
  assert.throws(() => unframeMessages(framed.subarray(0, 4)), /truncated message header/);
});
