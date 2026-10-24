// A JavaScript host for sourcekit-lsp-lite-reactor.wasm, the way a browser
// worker drives it: instantiate once, then one lsp_handle call per JSON-RPC
// message, and lsp_idle when no message is waiting. It is both the reference
// for the browser pack and a test (node:wasi stands in for the browser's WASI
// shim; the calls into the module are the same).
//
//   node --no-turbo-fast-api-calls reactor_host.mjs <reactor.wasm> [--dir guest=host ...]
//        [--root /guest/project] [--file name ...] [--compile] [-- compiler args]
//
// (Without that flag Node 22's node:wasi can segfault in a garbage collection
// started inside its fd_pread fast call; see Stage 53.)
//
// Without --root it only checks the lifecycle (initialize, shutdown, exit),
// which needs no sourcekitd answers. With --root it opens each --file, waits
// for diagnostics from lsp_idle, asks for a hover on the first identifier and
// renames something. With --compile (swift-frontend-lsp.wasm only) it then
// instantiates the same compiled module again and compiles the files to an
// object with swift_frontend_main, as a browser's runner would.

import { readFileSync } from 'node:fs';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { WASI } from 'node:wasi';
import { SwiftPluginHost } from '../../../plugin-host/swift-plugin-host.mjs';

const argv = process.argv.slice(2);
const wasmPath = argv.shift();
const preopens = {};
let root = null;
const files = [];
let compilerArgs = [];
let compile = false;
for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--dir') {
        const [guest, host] = argv[++i].split('=');
        preopens[guest] = host;
    } else if (argv[i] === '--root') root = argv[++i];
    else if (argv[i] === '--file') files.push(argv[++i]);
    else if (argv[i] === '--compile') compile = true;
    else if (argv[i] === '--') { compilerArgs = argv.slice(i + 1); break; }
}

const wasi = new WASI({ version: 'preview1', args: ['sourcekit-lsp-lite'], env: { TMPDIR: '/tmp' }, preopens });
const t0 = performance.now();
const module = await WebAssembly.compile(await readFile(wasmPath));

// A module built with macro support (patch 0003) imports its plugin server
// from the embedder: give each instance a SwiftPluginHost, which loads the wasm
// plugins the compiler names from the same directories the module sees.
const needsPluginHost = WebAssembly.Module.imports(module).some((i) => i.module === 'swift_plugin_host');
function hostPathOf(guestPath) {
    const guest = Object.keys(preopens)
        .filter((g) => guestPath === g || guestPath.startsWith(g.endsWith('/') ? g : `${g}/`))
        .sort((a, b) => b.length - a.length)[0];
    if (guest === undefined) throw new Error(`'${guestPath}' is outside every --dir`);
    return path.join(preopens[guest], path.posix.relative(guest, guestPath));
}
function instantiate(imports) {
    if (!needsPluginHost) return { instance: new WebAssembly.Instance(module, imports), host: null };
    const host = new SwiftPluginHost({
        loadPlugin: (libraryPath) => readFileSync(hostPathOf(libraryPath)),
        onStderr: (name, text) => process.stderr.write(`[${name}] ${text}`),
    });
    const instance = new WebAssembly.Instance(module, { ...imports, ...host.imports });
    host.attach(instance.exports.memory);
    return { instance, host };
}

const { instance } = instantiate(wasi.getImportObject());
wasi.initialize(instance); // runs _initialize: the reactor's constructors
const lsp = instance.exports;
console.log(`instantiated in ${Math.round(performance.now() - t0)} ms`);

const encoder = new TextEncoder();
const decoder = new TextDecoder();

/** Hands one message to the module and returns the messages it wrote. */
function call(fn, message) {
    let length;
    if (message === undefined) {
        length = lsp.lsp_idle();
    } else {
        const bytes = encoder.encode(JSON.stringify(message));
        const pointer = lsp.lsp_alloc(bytes.length);
        new Uint8Array(lsp.memory.buffer, pointer, bytes.length).set(bytes);
        length = lsp.lsp_handle(pointer, bytes.length);
        lsp.lsp_free(pointer);
    }
    // Read the output before the next call: it is overwritten.
    const text = decoder.decode(new Uint8Array(lsp.memory.buffer, lsp.lsp_output(), length));
    return text.split('\n').filter(Boolean).map((line) => JSON.parse(line));
}

let failures = 0;
const check = (ok, what, detail = '') => {
    console.log(`${ok ? 'PASS' : 'FAIL'}  ${what}${ok || detail === '' ? '' : `: ${JSON.stringify(detail)}`}`);
    if (!ok) failures++;
};

let id = 0;
const request = (method, params) => {
    const start = performance.now();
    const out = call('handle', { jsonrpc: '2.0', id: ++id, method, params });
    const reply = out.find((m) => m.id === id);
    console.log(`  ${method}: ${(performance.now() - start).toFixed(1)} ms`);
    return reply;
};
const notify = (method, params) => call('handle', { jsonrpc: '2.0', method, params });

const init = request('initialize', {
    processId: null,
    rootUri: root ? `file://${root}` : null,
    capabilities: {},
    initializationOptions: { compilerArgs },
});
check(init?.result?.serverInfo?.name === 'sourcekit-lsp-lite', 'initialize answers through lsp_output', init);
check(lsp.lsp_exit_code() === -1, 'the server runs until exit');

if (root) {
    for (const name of files) {
        const text = await readFile(`${preopens[root] ?? root}/${name}`, 'utf8').catch(() => null);
        check(text !== null, `read ${name}`);
        notify('textDocument/didOpen', { textDocument: { uri: `file://${root}/${name}`, languageId: 'swift', version: 1, text } });
    }
    // What a worker does from a timer when no message is waiting.
    const published = [];
    const start = performance.now();
    while (lsp.lsp_has_idle_work()) published.push(...call('idle'));
    console.log(`  diagnostics for ${files.length} files: ${(performance.now() - start).toFixed(1)} ms`);
    const diags = published.filter((m) => m.method === 'textDocument/publishDiagnostics');
    check(diags.length === files.length, 'lsp_idle publishes diagnostics for every open file', diags.length);
    check(diags.every((d) => !d.params.diagnostics.some((x) => x.severity === 1)), 'the project has no errors',
        diags.map((d) => d.params.diagnostics));
    const hover = request('textDocument/hover', {
        textDocument: { uri: `file://${root}/${files[0]}` }, position: { line: 0, character: 9 } });
    check(/struct Point/.test(hover?.result?.contents?.value ?? ''), 'hover answers from sourcekitd', hover);
    const highlight = request('textDocument/documentHighlight', {
        textDocument: { uri: `file://${root}/${files[0]}` }, position: { line: 0, character: 4 } });
    check((highlight?.result?.length ?? 0) >= 2, 'documentHighlight answers from relatedidents', highlight);
    const renamed = request('textDocument/rename', {
        textDocument: { uri: `file://${root}/${files[0]}` }, position: { line: 0, character: 4 }, newName: 'origin' });
    const edits = Object.values(renamed?.result?.changes ?? {}).flat();
    check(edits.length >= 2 && edits.every((e) => e.newText === 'origin'), 'rename edits every use', renamed);
}

check(request('shutdown', null)?.result === null, 'shutdown');
notify('exit', null);
check(lsp.lsp_exit_code() === 0, 'exit after shutdown is 0');

if (compile) {
    // A fresh instance of the module compiled above: one compile per
    // instance (src/frontend.cpp says why).
    const started = performance.now();
    const cwasi = new WASI({ version: 'preview1', args: ['swift-frontend'], env: { TMPDIR: '/tmp' }, preopens, returnOnExit: true });
    const { instance: compiler } = instantiate(cwasi.getImportObject());
    cwasi.initialize(compiler);
    const fe = compiler.exports;
    check(typeof fe.swift_frontend_main === 'function', 'the module exports swift_frontend_main');
    const output = '/tmp/reactor-host-main.o';
    const args = ['swift-frontend', '-frontend', '-c', ...files.map((f) => `${root}/${f}`),
        '-module-name', 'App', '-o', output, ...compilerArgs];
    const pointers = args.map((a) => {
        const bytes = encoder.encode(`${a}\0`);
        const p = fe.lsp_alloc(bytes.length);
        new Uint8Array(fe.memory.buffer, p, bytes.length).set(bytes);
        return p;
    });
    const argvPointer = fe.lsp_alloc(4 * pointers.length);
    new Uint32Array(fe.memory.buffer, argvPointer, pointers.length).set(pointers);
    let status;
    try {
        status = fe.swift_frontend_main(args.length, argvPointer);
    } catch (error) {
        status = `threw ${error}`;
    }
    console.log(`  swift_frontend_main -c: ${(performance.now() - started).toFixed(1)} ms`);
    check(status === 0, 'swift_frontend_main compiles the project', status);
    const object = await readFile(`${preopens['/tmp']}/reactor-host-main.o`).catch(() => null);
    check(object !== null && object.subarray(0, 4).equals(Buffer.from([0, 0x61, 0x73, 0x6d])),
        'it writes a wasm object file', object?.length);
}
process.exit(failures ? 1 : 0);
