// Firmware stdout capture (see stest/stdout_capture.hpp).
#include "stest/stdout_capture.hpp"
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <nlohmann/json.hpp>
#include "stest/node.hpp"
#include "stest/mmio.h"
#include "stest/runtime.hpp"

namespace stest {
namespace {

// Length of the valid UTF-8 sequence at s[i], or 0.
size_t utf8_len(const std::string& s, size_t i) {
  auto c = static_cast<unsigned char>(s[i]);
  size_t n = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
  if (n == 0 || i + n > s.size()) return 0;
  for (size_t k = 1; k < n; ++k)
    if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 0;
  if (n == 3) {
    auto c1 = static_cast<unsigned char>(s[i + 1]);
    if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) return 0;
  } else if (n == 4) {
    auto c1 = static_cast<unsigned char>(s[i + 1]);
    if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) return 0;
  }
  return n;
}

void write_fd(int fd, const char* p, size_t n) {
  while (n > 0) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return;  // reader gone: the events still carry the text
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
}

StdoutCapture* g_capture = nullptr;

#if defined(__APPLE__)
int cookie_write(void* c, const char* p, int n) {
  static_cast<StdoutCapture*>(c)->write(p, static_cast<size_t>(n));
  return n;
}
#elif defined(__linux__)
ssize_t cookie_write(void* c, const char* p, size_t n) {
  static_cast<StdoutCapture*>(c)->write(p, n);
  return static_cast<ssize_t>(n);
}
#endif

}  // namespace

std::string sanitize_utf8(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    size_t n = utf8_len(s, i);
    if (n) { out.append(s, i, n); i += n; }
    else { out += "\xEF\xBF\xBD"; ++i; }
  }
  return out;
}

void StdoutCapture::emit_line() {
  emit_event(runtime_clock().now_ns(), {{"kind", "stdout"}, {"periph", "stdout"}, {"text", sanitize_utf8(line_)}});
  line_.clear();
}

void StdoutCapture::write(const char* p, size_t n) {
  if (fd_ >= 0) write_fd(fd_, p, n);
  for (size_t i = 0; i < n; ++i) {
    char c = p[i];
    if (c == '\n') {
      if (!line_.empty() && line_.back() == '\r') line_.pop_back();
      emit_line();
      continue;
    }
    line_.push_back(c);
    if (line_.size() >= kMaxLine) emit_line();
  }
}

std::optional<std::string> StdoutCapture::take_partial() {
  if (line_.empty()) return std::nullopt;
  std::string s = sanitize_utf8(line_);
  line_.clear();
  return s;
}

void install_stdout_capture(bool forward_to_fd1) {
  if (g_capture) return;
  std::fflush(stdout);  // whatever the original stream still buffers goes out first
  auto* cap = new StdoutCapture(forward_to_fd1 ? 1 : -1);  // lives until the process exits
#if defined(__APPLE__)
  FILE* f = funopen(cap, nullptr, &cookie_write, nullptr, nullptr);
#elif defined(__linux__)
  cookie_io_functions_t io{};
  io.write = &cookie_write;
  FILE* f = fopencookie(cap, "w", io);
#else
#error "stdout capture: unsupported host (needs funopen or fopencookie)"
#endif
  if (!f) stest_rt_abort("cannot create the stdout capture stream");
  // Unbuffered: every stdio call reaches the capture at once, so a line is stamped with the
  // virtual time of the call that completed it.
  std::setvbuf(f, nullptr, _IONBF, 0);
  stdout = f;
  g_capture = cap;
}

std::optional<std::string> take_stdout_partial() {
  return g_capture ? g_capture->take_partial() : std::nullopt;
}

}  // namespace stest
