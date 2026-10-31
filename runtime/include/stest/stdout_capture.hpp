#pragma once
// Firmware stdout capture with virtual timestamps.
//
// A node process replaces the C `stdout` FILE with an unbuffered custom stream (macOS
// funopen, Linux fopencookie) before the firmware starts. Everything the firmware writes
// through `stdout` (printf, puts, putchar, fwrite/fputs(..., stdout), vprintf, ...) reaches
// StdoutCapture::write: bytes are assembled into lines and each '\n' emits the event
//   {"kind":"stdout","periph":"stdout","text":"<line without \n and a trailing \r>"}
// at the current virtual time (invalid UTF-8 replaced by U+FFFD; a line longer than
// kMaxLine bytes is emitted early), and the bytes are forwarded unchanged to fd 1 when
// forwarding is on (control mode: Python drains the pipe for Node.stdout_tail).
//
// Bypasses (not captured as events, but still reach fd 1): raw write(1, ...), output to a
// FILE* the firmware obtained from fdopen(1, ...) itself, and C++ iostreams in libraries that
// bind to the C stdout FILE before the node starts. Firmware is C and uses stdio, so in
// practice everything goes through the stream. stderr is never captured.
//
// Not on the MMIO hot path: the stream is only touched by firmware stdio calls.
#include <cstddef>
#include <optional>
#include <string>

namespace stest {

class StdoutCapture {
 public:
  static constexpr size_t kMaxLine = 4096;
  // forward_fd: where the raw bytes are copied (1 in control mode), or -1 for none.
  explicit StdoutCapture(int forward_fd) : fd_(forward_fd) {}
  void write(const char* p, size_t n);  // assemble lines, emit events, forward the bytes
  // Unterminated text written since the last '\n' (with a trailing '\r' dropped); clears it.
  std::optional<std::string> take_partial();

 private:
  void emit_line();
  int fd_;
  std::string line_;
};

// Replace the process's `stdout` with the capturing stream (idempotent). forward_to_fd1: copy
// the bytes to fd 1 as well. Call before the firmware thread starts.
void install_stdout_capture(bool forward_to_fd1);
// The installed capture's unterminated line, if any (for exit/abort reporting); clears it.
std::optional<std::string> take_stdout_partial();

// Replace invalid UTF-8 with U+FFFD (event text must be valid JSON strings).
std::string sanitize_utf8(const std::string& s);

}  // namespace stest
