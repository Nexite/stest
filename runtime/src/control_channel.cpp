// Control mode: serves the stest control protocol (stest/protocol.h) on one socket.
//
// Threading: the loop thread (this file) owns the socket and the Node. While it is
// inside Node::run_until the firmware thread runs and the loop thread is parked on the
// baton, so the socket has exactly one user at a time. The one exception is
// stest_rt_abort on the firmware thread: its hook (abort_hook) writes the ERROR frame
// with plain blocking send() calls on the same fd. That cannot interleave with a loop
// frame (the loop is parked mid-run_until, never mid-write) and does not need the loop
// thread, so it cannot deadlock.
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "stest/mmio.h"
#include "stest/node.hpp"
#include "stest/protocol.h"
#include "stest/runtime.hpp"

namespace stest {
namespace {

int g_fd = -1;

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;  // Linux: EPIPE instead of SIGPIPE
#else
constexpr int kSendFlags = 0;  // macOS: SO_NOSIGPIPE is set on the socket instead
#endif

bool write_all(const void* p, size_t n) {
  const char* c = static_cast<const char*>(p);
  while (n) {
    ssize_t w = ::send(g_fd, c, n, kSendFlags);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    c += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

// false on EOF/error before n bytes
bool read_all(void* p, size_t n) {
  char* c = static_cast<char*>(p);
  while (n) {
    ssize_t r = ::recv(g_fd, c, n, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    c += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

// Reads and drops n bytes in bounded chunks; false on EOF/error before n bytes.
bool discard(uint64_t n) {
  char scratch[64 * 1024];
  while (n) {
    size_t chunk = n < sizeof scratch ? static_cast<size_t>(n) : sizeof scratch;
    if (!read_all(scratch, chunk)) return false;
    n -= chunk;
  }
  return true;
}

// ---- little-endian payload builder / parser ----
struct Out {
  std::string b;
  void u8(uint8_t v) { b.push_back(static_cast<char>(v)); }
  void u16(uint16_t v) { le(v, 2); }
  void u32(uint32_t v) { le(v, 4); }
  void u64(uint64_t v) { le(v, 8); }
  void bytes(const void* p, size_t n) { b.append(static_cast<const char*>(p), n); }
  void le(uint64_t v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
};

struct In {
  const std::vector<uint8_t>& b;
  size_t off = 0;
  uint64_t le(int n) {
    if (b.size() - off < static_cast<size_t>(n))
      stest_rt_abort("control protocol: truncated payload");
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= uint64_t{b[off + i]} << (8 * i);
    off += static_cast<size_t>(n);
    return v;
  }
  uint16_t u16() { return static_cast<uint16_t>(le(2)); }
  uint32_t u32() { return static_cast<uint32_t>(le(4)); }
  uint64_t u64() { return le(8); }
  const uint8_t* bytes(size_t n) {
    if (b.size() - off < n) stest_rt_abort("control protocol: truncated payload");
    const uint8_t* p = b.data() + off;
    off += n;
    return p;
  }
  void end() {
    if (off != b.size()) stest_rt_abort("control protocol: %zu trailing payload bytes", b.size() - off);
  }
};

// Returns false if the peer is gone (the caller exits quietly).
bool send_msg(uint8_t type, const std::string& payload) {
  Out h;
  h.u32(static_cast<uint32_t>(payload.size()));
  h.u8(type);
  h.bytes(payload.data(), payload.size());
  return write_all(h.b.data(), h.b.size());
}

void send_or_exit(uint8_t type, const std::string& payload) {
  if (!send_msg(type, payload)) _exit(0);  // Python went away: nothing left to report to
}

void abort_hook(const char* msg) { (void)send_msg(STEST_MSG_ERROR, msg); }

// ---- fault-safe memory copy ----
// Copies through a pipe: the kernel validates both addresses (EFAULT instead of a
// SIGSEGV), so a bad READ_MEM/WRITE_MEM address is a recoverable request error.
int g_pipe[2] = {-1, -1};

bool safe_copy(void* dst, const void* src, size_t n) {
  if (g_pipe[0] < 0 && ::pipe(g_pipe) != 0) stest_rt_abort("control channel: pipe(): %s", strerror(errno));
  auto* d = static_cast<char*>(dst);
  auto* s = static_cast<const char*>(src);
  while (n) {
    size_t chunk = n < 4096 ? n : 4096;
    ssize_t w = ::write(g_pipe[1], s, chunk);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return false;
    size_t got = 0;
    bool ok = true;
    while (got < static_cast<size_t>(w)) {
      ssize_t r = ::read(g_pipe[0], d + got, static_cast<size_t>(w) - got);
      if (r < 0 && errno == EINTR) continue;
      if (r < 0) {  // bad destination: drain the pipe into a scratch buffer
        ok = false;
        char scratch[4096];
        while (got < static_cast<size_t>(w)) {
          ssize_t k = ::read(g_pipe[0], scratch, static_cast<size_t>(w) - got);
          if (k < 0 && errno == EINTR) continue;
          if (k <= 0) stest_rt_abort("control channel: pipe drain failed");
          got += static_cast<size_t>(k);
        }
        break;
      }
      if (r <= 0) stest_rt_abort("control channel: pipe read failed");
      got += static_cast<size_t>(r);
    }
    if (!ok) return false;
    d += w;
    s += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

std::string hex(uint64_t a) {
  char b[32];
  std::snprintf(b, sizeof b, "0x%" PRIx64, a);
  return b;
}

void handle_model_call(In& in) {
  uint16_t len = in.u16();
  const uint8_t* js = in.bytes(len);
  in.end();
  nlohmann::json req = nlohmann::json::parse(js, js + len, nullptr, /*allow_exceptions=*/false);
  if (req.is_discarded() || !req.is_object() || !req.contains("model") || !req["model"].is_string() ||
      !req.contains("op") || !req["op"].is_string())
    stest_rt_abort("control protocol: bad MODEL_CALL request");
  std::string model = req["model"].get<std::string>(), op = req["op"].get<std::string>();
  nlohmann::json args = req.value("args", nlohmann::json::object());
  Peripheral* p = find_model(model);
  if (!p) {
    send_or_exit(STEST_MSG_REQ_ERROR, "MODEL_CALL: unknown model '" + model + "'");
    return;
  }
  nlohmann::json result;
  try {
    result = p->call(op, args);
  } catch (const UnknownOp& e) {
    send_or_exit(STEST_MSG_REQ_ERROR, "MODEL_CALL " + model + ": " + e.what());
    return;
  } catch (const std::exception& e) {
    send_or_exit(STEST_MSG_REQ_ERROR, "MODEL_CALL " + model + "." + op + ": " + e.what());
    return;
  }
  send_or_exit(STEST_MSG_MODEL_RESULT, result.dump());
}

}  // namespace

void run_control_channel(int fd, const ChipDesc& chip, FirmwareMain fw, const void* anchor) {
  g_fd = fd;
#if defined(SO_NOSIGPIPE)
  int one = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  set_abort_hook(&abort_hook);
  if (const char* c = std::getenv("STEST_ACCESS_COST_NS")) {
    char* end = nullptr;
    unsigned long long v = std::strtoull(c, &end, 10);
    if (!*c || *end) stest_rt_abort("bad STEST_ACCESS_COST_NS '%s'", c);
    set_access_cost_ns(v);
  }

  try {
    // Never destroyed: the process always ends with _exit while the firmware is parked.
    auto* node = new Node(chip);
    node->start(fw);

    Out hello;
    hello.u32(STEST_PROTO_VERSION);
    hello.u64(reinterpret_cast<uintptr_t>(anchor));
    send_or_exit(STEST_MSG_HELLO, hello.b);

    std::vector<uint8_t> payload;
    for (;;) {
      uint8_t hdr[5];
      if (!read_all(hdr, sizeof hdr)) {  // EOF: Python closed the channel
        std::fflush(stdout);
        _exit(0);
      }
      uint32_t len = uint32_t{hdr[0]} | uint32_t{hdr[1]} << 8 | uint32_t{hdr[2]} << 16 |
                     uint32_t{hdr[3]} << 24;
      uint8_t type = hdr[4];
      if (len > STEST_MAX_PAYLOAD) {  // a request error: drop the payload, keep serving
        if (!discard(len)) stest_rt_abort("control protocol: truncated message");
        send_or_exit(STEST_MSG_REQ_ERROR, "control protocol: payload of " + std::to_string(len) +
                                              " bytes exceeds the maximum of " +
                                              std::to_string(STEST_MAX_PAYLOAD) + " (message type " +
                                              std::to_string(type) + " discarded)");
        continue;
      }
      payload.resize(len);
      if (len && !read_all(payload.data(), len)) stest_rt_abort("control protocol: truncated message");
      In in{payload};

      switch (type) {
        case STEST_MSG_RUN_UNTIL: {
          uint64_t t = in.u64();
          in.end();
          if (node->run_until(t) == Node::Stop::Exited)
            stest_rt_abort("firmware main returned %d at t=%" PRIu64 " ns", node->exit_code(),
                           node->now_ns());
          std::vector<Event> evs = take_events();
          Out o;
          o.u64(node->now_ns());
          o.u32(static_cast<uint32_t>(evs.size()));
          for (const Event& e : evs) {
            std::string js = e.data.dump();
            if (js.size() > 0xFFFF) stest_rt_abort("event JSON too long (%zu bytes)", js.size());
            o.u64(e.t_ns);
            o.u16(static_cast<uint16_t>(js.size()));
            o.bytes(js.data(), js.size());
          }
          send_or_exit(STEST_MSG_STOPPED, o.b);
          break;
        }
        case STEST_MSG_READ_MEM: {
          uint64_t addr = in.u64();
          uint32_t n = in.u32();
          in.end();
          if (n > STEST_MAX_PAYLOAD) {
            send_or_exit(STEST_MSG_REQ_ERROR, "read_mem: " + std::to_string(n) +
                                                  " bytes exceeds the maximum of " +
                                                  std::to_string(STEST_MAX_PAYLOAD));
            break;
          }
          std::string buf(n, '\0');
          if (n && !safe_copy(buf.data(), reinterpret_cast<const void*>(addr), n))
            send_or_exit(STEST_MSG_REQ_ERROR,
                         "read_mem: cannot read " + std::to_string(n) + " bytes at " + hex(addr));
          else
            send_or_exit(STEST_MSG_MEM, buf);
          break;
        }
        case STEST_MSG_WRITE_MEM: {
          uint64_t addr = in.u64();
          uint32_t n = in.u32();
          const uint8_t* data = in.bytes(n);
          in.end();
          if (n && !safe_copy(reinterpret_cast<void*>(addr), data, n))
            send_or_exit(STEST_MSG_REQ_ERROR,
                         "write_mem: cannot write " + std::to_string(n) + " bytes at " + hex(addr));
          else
            send_or_exit(STEST_MSG_OK, "");
          break;
        }
        case STEST_MSG_MODEL_CALL:
          handle_model_call(in);
          break;
        case STEST_MSG_SHUTDOWN:
          in.end();
          std::fflush(stdout);
          std::fflush(stderr);
          _exit(0);
        default:
          stest_rt_abort("control protocol: unexpected message type %u", static_cast<unsigned>(type));
      }
    }
  } catch (const std::exception& e) {
    stest_rt_abort("control channel: %s", e.what());
  } catch (...) {
    stest_rt_abort("control channel: unknown exception");
  }
}

}  // namespace stest
