// LSP base protocol framing: "Content-Length: N\r\n\r\n" and N bytes.
//
// Pure: bytes go in, message bodies come out. The stdio front end feeds it
// whatever read() returned, however the stream happened to be split.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace lite {

class FrameDecoder {
public:
  /// Appends bytes read from the stream.
  void feed(std::string_view Bytes);

  /// The next complete message body, if one has arrived. A header block
  /// without a usable Content-Length is skipped and recorded in lastError().
  [[nodiscard]] std::optional<std::string> next();

  /// What was wrong with the last header block that had to be skipped, or
  /// empty.
  [[nodiscard]] const std::string &lastError() const noexcept { return Error; }

  /// Bytes received and not yet returned as a message.
  [[nodiscard]] std::size_t buffered() const noexcept { return Buffer.size() - Consumed; }

private:
  std::string Buffer;
  std::size_t Consumed = 0;
  std::string Error;
  void compact();
};

/// \p Body with its header, ready to write.
[[nodiscard]] std::string frame(std::string_view Body);

/// The value of a Content-Length header in \p Headers (case-insensitive name),
/// or nullopt if there is none or it is not a non-negative number.
[[nodiscard]] std::optional<std::size_t> contentLength(std::string_view Headers);

} // namespace lite
