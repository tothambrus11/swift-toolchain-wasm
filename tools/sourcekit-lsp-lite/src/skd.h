// The only part of the server that touches sourcekitd's C API.
//
// Requests go in as JSON (requests.h), responses come back as JSON with the
// same shape sourcekitd_variant_json_description_copy prints: keys and uids
// by name. Everything that interprets a response is then a pure function of
// JSON (translate.h), tested against recorded responses.
//
// Only the synchronous half of the API is used: sourcekitd_send_request_sync.
// The asynchronous half and the notification handler take blocks, which a
// client built without -fblocks cannot make; on a single-threaded WASI host
// they would be answered through the same cooperative queue anyway.

#pragma once

#include "json.h"

#include <string>

namespace lite::skd {

/// A response: the value, or what went wrong.
struct Reply {
  bool OK = false;
  json::Value Value;
  std::string Error;
};

/// Owns sourcekitd's process-wide state: initializes it on construction and
/// shuts it down on destruction. Exactly one may exist at a time.
class Session {
public:
  Session();
  ~Session();
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  Session(Session &&) = delete;
  Session &operator=(Session &&) = delete;

  /// Sends \p Request and waits for the answer. On WASI the wait is where
  /// queued work runs (Concurrency-cooperative.cpp), so this returns once the
  /// request has been answered on this, the only, thread. When
  /// $SOURCEKIT_LSP_LITE_RECORD names a file, each request and its reply are
  /// appended to it as one JSON line: that is how test fixtures are made.
  [[nodiscard]] Reply send(const json::Value &Request) const;
};

} // namespace lite::skd
