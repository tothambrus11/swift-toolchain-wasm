// A stand-in for a Swift macro plugin, for testing swift-plugin-host.mjs
// without a Swift toolchain. It speaks the same guest ABI as swift-syntax's
// SwiftCompilerPlugin on WASI: main() returns at once, and every call to the
// export `swift_wasm_macro_v1_pump` reads one framed message from stdin and
// writes one framed response to stdout. It understands two requests by
// substring: getCapability, and any expansion, which it answers with the
// string literal "expanded by echo-plugin". A request containing "trapMe"
// traps, to test recovery.
//
// Rebuild with ./build-fixtures.sh (needs wasi-sdk).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int requestCount = 0;

static int readAll(void *buffer, size_t length) {
  size_t done = 0;
  while (done < length) {
    ssize_t n = read(0, (char *)buffer + done, length - done);
    if (n <= 0)
      return -1;
    done += (size_t)n;
  }
  return 0;
}

static void writeFramed(const char *text) {
  uint64_t length = strlen(text);
  write(1, &length, sizeof length); // wasm32 is little-endian
  write(1, text, (size_t)length);
}

__attribute__((export_name("swift_wasm_macro_v1_pump"))) void pump(void) {
  static char message[65536];
  uint64_t length = 0;
  if (readAll(&length, sizeof length) != 0 || length >= sizeof message)
    return;
  if (readAll(message, (size_t)length) != 0)
    return;
  message[length] = '\0';
  requestCount += 1;

  if (strstr(message, "trapMe"))
    __builtin_trap();
  if (strstr(message, "\"getCapability\"")) {
    writeFramed("{\"getCapabilityResult\":{\"capability\":{\"protocolVersion\":7}}}");
    return;
  }
  char reply[256];
  snprintf(reply, sizeof reply,
           "{\"expandMacroResult\":{\"expandedSource\":"
           "\"\\\"expanded by echo-plugin, request %d\\\"\",\"diagnostics\":[]}}",
           requestCount);
  fprintf(stderr, "echo-plugin: request %d\n", requestCount);
  writeFramed(reply);
}

int main(void) { return 0; }
