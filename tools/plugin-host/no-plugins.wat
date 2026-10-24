;; The "swift_plugin_host" imports of a macro-enabled swift-frontend.wasm,
;; sourcekitd or sourcekit-lsp-lite (patch 0003, lib/AST/PluginRegistry.cpp),
;; for a WASI runtime that cannot run JavaScript:
;;
;;   wasmtime run --preload swift_plugin_host=no-plugins.wat swift-frontend.wasm ...
;;
;; It is an embedder with no plugins. Every message fails without a response,
;; so the module instantiates and works for code that uses no macros, and a
;; macro is reported against its use ("plugin ... failed") instead of the
;; module refusing to start. The frontend initialises the response length to
;; 0 before the call, and this module has no memory of its own to write it
;; with, so returning nonzero is the whole answer. Macros need
;; swift-plugin-host.mjs.
(module
  (func (export "handle_message") (param $request i32) (param $length i32)
                                  (param $responseLength i32) (result i32)
    i32.const 1)
  (func (export "take_response") (param $buffer i32)))
