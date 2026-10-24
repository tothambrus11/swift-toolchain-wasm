// Runs Swift macro plugins, compiled to WebAssembly, for a swift-frontend.wasm
// built with macro support.
//
// Such a frontend has no plugin server process or library to talk to. It asks
// its embedder instead, through two wasm imports in the module
// "swift_plugin_host" (see the WASI arm of lib/AST/PluginRegistry.cpp in the
// patch series). This module is that embedder's side: it answers the plugin
// server's own messages (getCapability, loadPluginLibrary) and forwards each
// macro expansion to the wasm plugin that provides the macro's module.
//
// A plugin is a WASI command built with swift-syntax's SwiftCompilerPlugin for
// wasm32-wasip1 (swift-syntax#2623). Its ABI: run `_start` once, then for every
// request write the message, framed by its byte length as a little-endian
// 64-bit integer, to the plugin's stdin and call its export
// `swift_wasm_macro_v1_pump`, which reads one message and writes the framed
// response to stdout. Everything here is synchronous, because the frontend is
// blocked inside an import call while a macro expands, so it works in Node and
// in a browser worker alike. No dependencies; no Node APIs.

const WASI_ESUCCESS = 0;
const WASI_EBADF = 8;
const WASI_EINVAL = 28;
const WASI_ENOSYS = 52;
const WASI_FILETYPE_CHARACTER_DEVICE = 2;

const encoder = new TextEncoder();
const decoder = new TextDecoder();

/** Thrown out of a guest's `proc_exit` to unwind it. */
export class GuestExit extends Error {
  /** @param {number} code */
  constructor(code) {
    super(`plugin exited with code ${code}`);
    this.code = code;
  }
}

/**
 * Just enough WASI for a macro plugin: stdin is a queue the host fills, stdout
 * is a buffer the host drains, stderr goes to `onStderr`. There is no
 * filesystem, no arguments beyond the program name, and no environment; any
 * call not listed fails with ENOSYS rather than being missing, so a plugin
 * that imports more still instantiates.
 */
class WasiGuest {
  /**
   * @param {string} programName argv[0]
   * @param {(text: string) => void} onStderr
   */
  constructor(programName, onStderr) {
    /** @type {WebAssembly.Memory | null} */
    this.memory = null;
    this.args = [encoder.encode(`${programName}\0`)];
    this.onStderr = onStderr;
    /** Bytes the guest has not read yet. */
    this.stdin = new Uint8Array(0);
    /** @type {Uint8Array[]} what the guest wrote to stdout since the last take */
    this.stdout = [];

    const self = this;
    const implemented = {
      args_sizes_get(argcPtr, bufSizePtr) {
        const view = self.view();
        view.setUint32(argcPtr, self.args.length, true);
        view.setUint32(bufSizePtr, self.args.reduce((n, a) => n + a.length, 0), true);
        return WASI_ESUCCESS;
      },
      args_get(argvPtr, bufPtr) {
        const view = self.view();
        let offset = bufPtr;
        self.args.forEach((arg, i) => {
          view.setUint32(argvPtr + 4 * i, offset, true);
          self.bytes().set(arg, offset);
          offset += arg.length;
        });
        return WASI_ESUCCESS;
      },
      environ_sizes_get(countPtr, bufSizePtr) {
        const view = self.view();
        view.setUint32(countPtr, 0, true);
        view.setUint32(bufSizePtr, 0, true);
        return WASI_ESUCCESS;
      },
      environ_get() {
        return WASI_ESUCCESS;
      },
      clock_res_get(_id, resolutionPtr) {
        self.view().setBigUint64(resolutionPtr, 1000n, true);
        return WASI_ESUCCESS;
      },
      clock_time_get(id, _precision, timePtr) {
        // Realtime for 0, a monotonic clock otherwise; both in nanoseconds.
        const ms = id === 0 ? Date.now() : globalThis.performance?.now() ?? Date.now();
        self.view().setBigUint64(timePtr, BigInt(Math.round(ms * 1e6)), true);
        return WASI_ESUCCESS;
      },
      random_get(bufPtr, length) {
        const bytes = self.bytes();
        // getRandomValues refuses more than 64 KiB per call.
        for (let done = 0; done < length; done += 65536) {
          globalThis.crypto.getRandomValues(
            bytes.subarray(bufPtr + done, bufPtr + Math.min(length, done + 65536)),
          );
        }
        return WASI_ESUCCESS;
      },
      fd_fdstat_get(fd, statPtr) {
        if (fd > 2) return WASI_EBADF;
        const view = self.view();
        // filetype u8, flags u16, rights_base u64, rights_inheriting u64
        for (let i = 0; i < 24; i += 1) view.setUint8(statPtr + i, 0);
        view.setUint8(statPtr, WASI_FILETYPE_CHARACTER_DEVICE);
        view.setBigUint64(statPtr + 8, 0xffffffffffffffffn, true);
        view.setBigUint64(statPtr + 16, 0xffffffffffffffffn, true);
        return WASI_ESUCCESS;
      },
      fd_prestat_get() {
        return WASI_EBADF; // no preopened directories
      },
      fd_prestat_dir_name() {
        return WASI_EBADF;
      },
      fd_close(fd) {
        return fd <= 2 ? WASI_ESUCCESS : WASI_EBADF;
      },
      fd_seek(fd) {
        return fd <= 2 ? WASI_EINVAL : WASI_EBADF; // streams do not seek
      },
      fd_read(fd, iovsPtr, iovsLength, nreadPtr) {
        if (fd !== 0) return WASI_EBADF;
        const view = self.view();
        const bytes = self.bytes();
        let total = 0;
        for (let i = 0; i < iovsLength && self.stdin.length > 0; i += 1) {
          const ptr = view.getUint32(iovsPtr + 8 * i, true);
          const len = view.getUint32(iovsPtr + 8 * i + 4, true);
          const n = Math.min(len, self.stdin.length);
          bytes.set(self.stdin.subarray(0, n), ptr);
          self.stdin = self.stdin.subarray(n);
          total += n;
        }
        view.setUint32(nreadPtr, total, true);
        return WASI_ESUCCESS; // 0 bytes read is end of input
      },
      fd_write(fd, iovsPtr, iovsLength, nwrittenPtr) {
        if (fd !== 1 && fd !== 2) return WASI_EBADF;
        const view = self.view();
        const bytes = self.bytes();
        let total = 0;
        for (let i = 0; i < iovsLength; i += 1) {
          const ptr = view.getUint32(iovsPtr + 8 * i, true);
          const len = view.getUint32(iovsPtr + 8 * i + 4, true);
          const chunk = bytes.slice(ptr, ptr + len);
          if (fd === 1) self.stdout.push(chunk);
          else self.onStderr(decoder.decode(chunk));
          total += len;
        }
        view.setUint32(nwrittenPtr, total, true);
        return WASI_ESUCCESS;
      },
      sched_yield() {
        return WASI_ESUCCESS;
      },
      proc_exit(code) {
        throw new GuestExit(code);
      },
    };
    /** The import namespace: anything not implemented answers ENOSYS. */
    this.imports = new Proxy(implemented, {
      get(target, name) {
        return name in target ? target[name] : () => WASI_ENOSYS;
      },
      has() {
        return true;
      },
    });
  }

  view() {
    return new DataView(this.memory.buffer);
  }

  bytes() {
    return new Uint8Array(this.memory.buffer);
  }

  /** Everything written to stdout since the last call, as one array. */
  takeStdout() {
    const length = this.stdout.reduce((n, c) => n + c.length, 0);
    const out = new Uint8Array(length);
    let offset = 0;
    for (const chunk of this.stdout) {
      out.set(chunk, offset);
      offset += chunk.length;
    }
    this.stdout = [];
    return out;
  }
}

/** Prefix a message with its length as a little-endian u64, the plugin wire format. */
export function frameMessage(text) {
  const body = encoder.encode(text);
  const framed = new Uint8Array(8 + body.length);
  new DataView(framed.buffer).setBigUint64(0, BigInt(body.length), true);
  framed.set(body, 8);
  return framed;
}

/**
 * Split framed bytes into messages. Throws if the bytes do not end exactly at
 * a message boundary, since a pump must answer with whole messages.
 */
export function unframeMessages(bytes) {
  const messages = [];
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let offset = 0;
  while (offset < bytes.length) {
    if (offset + 8 > bytes.length) throw new Error("truncated message header from plugin");
    const length = Number(view.getBigUint64(offset, true));
    offset += 8;
    if (offset + length > bytes.length) throw new Error("truncated message from plugin");
    messages.push(decoder.decode(bytes.subarray(offset, offset + length)));
    offset += length;
  }
  return messages;
}

/** One instantiated wasm macro plugin. */
export class MacroPlugin {
  /**
   * @param {WebAssembly.Module} module
   * @param {{ name: string, onStderr: (text: string) => void }} options
   */
  constructor(module, { name, onStderr }) {
    this.name = name;
    this.wasi = new WasiGuest(name, onStderr);
    this.instance = new WebAssembly.Instance(module, {
      wasi_snapshot_preview1: this.wasi.imports,
    });
    const { memory, _start, swift_wasm_macro_v1_pump: pump } = this.instance.exports;
    if (!(memory instanceof WebAssembly.Memory)) throw new Error(`${name}: exports no memory`);
    if (typeof pump !== "function") {
      throw new Error(`${name}: not a Swift macro plugin (no swift_wasm_macro_v1_pump export)`);
    }
    this.wasi.memory = memory;
    this.pump = pump;
    // main() registers the message handler and returns; the plugin then lives
    // on between pumps. A plugin that exits 0 from main is fine too.
    try {
      if (typeof _start === "function") _start();
    } catch (error) {
      if (!(error instanceof GuestExit) || error.code !== 0) throw error;
    }
  }

  /** Send one JSON request and return the plugin's JSON response. */
  request(json) {
    this.wasi.stdin = frameMessage(json);
    this.pump();
    const responses = unframeMessages(this.wasi.takeStdout());
    if (responses.length !== 1) {
      throw new Error(`${this.name}: expected one response, got ${responses.length}`);
    }
    return responses[0];
  }
}

function errorDiagnostic(message) {
  return {
    message,
    severity: "error",
    position: { fileName: "", offset: 0 },
    highlights: [],
    notes: [],
    fixIts: [],
  };
}

/**
 * The plugin server a macro-enabled swift-frontend.wasm talks to.
 *
 * `loadPlugin(libraryPath, moduleName)` returns the plugin for
 * `-load-plugin-library <libraryPath>` (or a `-plugin-path` entry) as a
 * `WebAssembly.Module` or as its bytes. It is called synchronously, from inside
 * the frontend; in a browser, compile modules ahead of time and return them,
 * since main threads refuse to compile large modules synchronously. The path is
 * the one the compiler saw, in its own (WASI) filesystem.
 */
export class SwiftPluginHost {
  /**
   * @param {{
   *   loadPlugin: (libraryPath: string, moduleName: string) => WebAssembly.Module | BufferSource,
   *   onStderr?: (pluginName: string, text: string) => void,
   *   trace?: (direction: "->" | "<-", json: string) => void,
   * }} options
   */
  constructor({ loadPlugin, onStderr = () => {}, trace = null }) {
    this.loadPlugin = loadPlugin;
    this.onStderr = onStderr;
    this.trace = trace;
    /**
     * @type {Map<string, MacroPlugin>} by library path. One module can provide
     * several macro modules (a bundle sharing one swift-syntax); it is
     * instantiated once and serves them all.
     */
    this.plugins = new Map();
    /** @type {Map<string, string>} macro module name -> library path */
    this.libraryPaths = new Map();
    /** The compiler's getCapability request, replayed to every plugin it loads. */
    this.capabilityRequest = null;
    /** @type {WebAssembly.Memory | null} the frontend's memory */
    this.memory = null;
    /** @type {Uint8Array | null} the response the frontend has not taken yet */
    this.pending = null;

    const host = this;
    /** Pass to `WebAssembly.instantiate` next to the WASI imports. */
    this.imports = {
      swift_plugin_host: {
        handle_message(requestPtr, requestLength, responseLengthPtr) {
          const memory = host.requireMemory();
          const request = decoder.decode(
            new Uint8Array(memory.buffer, requestPtr, requestLength).slice(),
          );
          const { ok, response } = host.handleMessage(request);
          host.pending = encoder.encode(response);
          new DataView(memory.buffer).setUint32(responseLengthPtr, host.pending.length, true);
          return ok ? 0 : 1;
        },
        take_response(bufferPtr) {
          const memory = host.requireMemory();
          new Uint8Array(memory.buffer).set(host.pending ?? new Uint8Array(0), bufferPtr);
          host.pending = null;
        },
      },
    };
  }

  /** Give the host the frontend's memory once it is instantiated. */
  attach(memory) {
    this.memory = memory;
  }

  requireMemory() {
    if (!this.memory) throw new Error("SwiftPluginHost.attach(memory) was not called");
    return this.memory;
  }

  /**
   * Handle one message from the compiler. Returns `ok: false` with an error
   * text when the message could not be handled at all; the compiler reports it
   * against the macro. Expansion errors a plugin reports are ordinary replies.
   */
  handleMessage(request) {
    this.trace?.("->", request);
    let result;
    try {
      result = { ok: true, response: this.reply(JSON.parse(request), request) };
    } catch (error) {
      result = { ok: false, response: error instanceof Error ? error.message : String(error) };
    }
    this.trace?.("<-", result.response);
    return result;
  }

  reply(message, request) {
    if ("getCapability" in message) {
      this.capabilityRequest = request;
      const requested = message.getCapability?.capability?.protocolVersion;
      return JSON.stringify({
        getCapabilityResult: {
          capability: {
            protocolVersion: Number.isInteger(requested) ? requested : 7,
            features: ["load-plugin-library"],
          },
        },
      });
    }
    if ("loadPluginLibrary" in message) {
      const { libraryPath, moduleName } = message.loadPluginLibrary;
      try {
        this.load(libraryPath, moduleName);
        return JSON.stringify({ loadPluginLibraryResult: { loaded: true, diagnostics: [] } });
      } catch (error) {
        const text = `could not load macro plugin '${libraryPath}': ${error?.message ?? error}`;
        return JSON.stringify({
          loadPluginLibraryResult: { loaded: false, diagnostics: [errorDiagnostic(text)] },
        });
      }
    }
    const [kind] = Object.keys(message);
    const moduleName = message[kind]?.macro?.moduleName;
    if (typeof moduleName !== "string") throw new Error(`unexpected plugin message '${kind}'`);
    return this.forward(moduleName, request);
  }

  load(libraryPath, moduleName) {
    if (this.plugins.has(libraryPath)) {
      this.libraryPaths.set(moduleName, libraryPath);
      return;
    }
    const source = this.loadPlugin(libraryPath, moduleName);
    const module = source instanceof WebAssembly.Module ? source : new WebAssembly.Module(source);
    const name = libraryPath.split("/").pop() ?? libraryPath;
    const plugin = new MacroPlugin(module, {
      name,
      onStderr: (text) => this.onStderr(name, text),
    });
    // The plugin learns the compiler's protocol version the way an executable
    // plugin would; its answer is for the compiler, which already has ours.
    if (this.capabilityRequest) plugin.request(this.capabilityRequest);
    this.plugins.set(libraryPath, plugin);
    this.libraryPaths.set(moduleName, libraryPath);
  }

  forward(moduleName, request) {
    const libraryPath = this.libraryPaths.get(moduleName);
    const plugin = libraryPath === undefined ? undefined : this.plugins.get(libraryPath);
    if (!plugin) throw new Error(`no plugin loaded for macro module '${moduleName}'`);
    try {
      return plugin.request(request);
    } catch (error) {
      // A trap leaves the instance unusable; start it afresh for the next
      // macro. Every module it served is served by the new instance.
      this.plugins.delete(libraryPath);
      try {
        this.load(libraryPath, moduleName);
      } catch {
        // Reported when the next expansion finds no plugin.
      }
      throw new Error(`macro plugin '${plugin.name}' failed on a '${moduleName}' macro: ${error?.message ?? error}`);
    }
  }
}
