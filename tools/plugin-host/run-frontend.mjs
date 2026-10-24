#!/usr/bin/env node
// Run swift-frontend.wasm under Node's WASI with the macro plugin host attached:
// the reference embedder for a macro-enabled compiler, and what
// scripts/57-macros-smoke-test.sh drives.
//
//   node run-frontend.mjs [--dir HOST::GUEST]... [--trace] swift-frontend.wasm ARGS...
//
// --dir preopens HOST at GUEST inside the compiler, like wasmtime's flag of the
// same name. A plugin path the compiler names (from -load-plugin-library, or
// found on a -plugin-path) is a guest path; it is read through the same
// mapping, so a plugin is wherever the compiler was told it is. --trace prints
// every message between the compiler and the plugins to stderr.
//
// Exits with the compiler's exit status.
import { readFileSync } from "node:fs";
import path from "node:path";
import process from "node:process";
import { WASI } from "node:wasi";

import { SwiftPluginHost } from "./swift-plugin-host.mjs";

function usage(message) {
  process.stderr.write(`run-frontend: ${message}\n`);
  process.stderr.write("usage: run-frontend.mjs [--dir HOST::GUEST]... [--trace] swift-frontend.wasm ARGS...\n");
  process.exit(2);
}

const preopens = {};
let trace = false;
const argv = process.argv.slice(2);
while (argv.length > 0 && argv[0].startsWith("--")) {
  const flag = argv.shift();
  if (flag === "--trace") {
    trace = true;
  } else if (flag === "--dir") {
    const spec = argv.shift() ?? usage("--dir needs HOST::GUEST");
    const [host, guest = host] = spec.split("::");
    preopens[guest] = path.resolve(host);
  } else {
    usage(`unknown option ${flag}`);
  }
}
const frontendPath = argv.shift() ?? usage("no swift-frontend.wasm given");

/** The host path behind a guest path, through the longest preopen that contains it. */
function hostPathOf(guestPath) {
  const normal = path.posix.normalize(guestPath);
  let best = null;
  for (const guest of Object.keys(preopens)) {
    const prefix = guest.endsWith("/") ? guest : `${guest}/`;
    if ((normal === guest || normal.startsWith(prefix)) && (best === null || guest.length > best.length)) {
      best = guest;
    }
  }
  if (best === null) throw new Error(`'${guestPath}' is outside every --dir the compiler was given`);
  return path.join(preopens[best], path.posix.relative(best, normal));
}

const compiled = new Map();
const host = new SwiftPluginHost({
  loadPlugin(libraryPath) {
    const file = hostPathOf(libraryPath);
    let module = compiled.get(file);
    if (module === undefined) {
      module = new WebAssembly.Module(readFileSync(file));
      compiled.set(file, module);
    }
    return module;
  },
  onStderr: (name, text) => process.stderr.write(`[${name}] ${text}`),
  trace: trace ? (direction, json) => process.stderr.write(`[plugin-host] ${direction} ${json}\n`) : null,
});

const wasi = new WASI({
  version: "preview1",
  args: [path.basename(frontendPath, ".wasm"), ...argv],
  env: { ...process.env },
  preopens,
  returnOnExit: true,
});

const module = new WebAssembly.Module(readFileSync(frontendPath));
const instance = new WebAssembly.Instance(module, {
  ...wasi.getImportObject(),
  ...host.imports,
});
host.attach(instance.exports.memory);
process.exitCode = wasi.start(instance);
