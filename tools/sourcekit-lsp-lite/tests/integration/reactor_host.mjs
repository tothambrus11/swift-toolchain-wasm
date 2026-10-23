// A JavaScript host for sourcekit-lsp-lite-reactor.wasm, the way a browser
// worker drives it: instantiate once, then one lsp_handle call per JSON-RPC
// message, and lsp_idle when no message is waiting. It is both the reference
// for the browser pack and a test (node:wasi stands in for the browser's WASI
// shim; the calls into the module are the same).
//
//   node reactor_host.mjs <reactor.wasm> [--dir guest=host ...] [--root /guest/project]
//        [--file name ...] [-- compiler args]
//
// Without --root it only checks the lifecycle (initialize, shutdown, exit),
// which needs no sourcekitd answers. With --root it opens each --file, waits
// for diagnostics from lsp_idle and asks for a hover on the first identifier.

import { readFile } from 'node:fs/promises';
import { WASI } from 'node:wasi';

const argv = process.argv.slice(2);
const wasmPath = argv.shift();
const preopens = {};
let root = null;
const files = [];
let compilerArgs = [];
for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--dir') {
        const [guest, host] = argv[++i].split('=');
        preopens[guest] = host;
    } else if (argv[i] === '--root') root = argv[++i];
    else if (argv[i] === '--file') files.push(argv[++i]);
    else if (argv[i] === '--') { compilerArgs = argv.slice(i + 1); break; }
}

const wasi = new WASI({ version: 'preview1', args: ['sourcekit-lsp-lite'], env: { TMPDIR: '/tmp' }, preopens });
const t0 = performance.now();
const module = await WebAssembly.compile(await readFile(wasmPath));
const instance = await WebAssembly.instantiate(module, wasi.getImportObject());
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
}

check(request('shutdown', null)?.result === null, 'shutdown');
notify('exit', null);
check(lsp.lsp_exit_code() === 0, 'exit after shutdown is 0');
process.exit(failures ? 1 : 0);
