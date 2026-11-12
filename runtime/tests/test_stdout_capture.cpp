// StdoutCapture: line assembly into virtual-timestamped "stdout" events + raw forwarding.
#include <doctest/doctest.h>
#include <unistd.h>
#include <string>
#include "stest/node.hpp"
#include "stest/runtime.hpp"
#include "stest/stdout_capture.hpp"
using namespace stest;

TEST_CASE("stdout capture: lines become stdout events at the virtual time of their newline") {
  (void)take_events();
  int fds[2];
  REQUIRE(pipe(fds) == 0);
  StdoutCapture cap(fds[1]);
  runtime_clock().advance(1000);
  const uint64_t t0 = runtime_clock().now_ns();
  cap.write("hel", 3);
  runtime_clock().advance(500);
  cap.write("lo\r\nwor", 7);  // CRLF: '\r' dropped
  cap.write("ld\n\n", 4);     // an empty line is a line
  cap.write("bad \xff\n", 6);  // invalid UTF-8 replaced
  cap.write("tail", 4);
  auto ev = take_events();
  REQUIRE(ev.size() == 4);
  CHECK(ev[0].t_ns == t0 + 500);
  CHECK(ev[0].data == nlohmann::json{{"kind", "stdout"}, {"periph", "stdout"}, {"text", "hello"}});
  CHECK(ev[1].data["text"] == "world");
  CHECK(ev[2].data["text"] == "");
  CHECK(ev[3].data["text"] == "bad \xEF\xBF\xBD");
  CHECK(cap.take_partial() == std::optional<std::string>("tail"));
  CHECK_FALSE(cap.take_partial().has_value());
  // the raw bytes were forwarded unchanged
  close(fds[1]);
  std::string raw;
  char buf[64];
  for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;) raw.append(buf, size_t(n));
  close(fds[0]);
  CHECK(raw == "hello\r\nworld\n\nbad \xff\ntail");
}

TEST_CASE("stdout capture: an overlong line is emitted early; no forwarding with fd -1") {
  (void)take_events();
  StdoutCapture cap(-1);
  std::string big(StdoutCapture::kMaxLine + 10, 'x');
  big += '\n';
  cap.write(big.data(), big.size());
  auto ev = take_events();
  REQUIRE(ev.size() == 2);
  CHECK(ev[0].data["text"].get<std::string>().size() == StdoutCapture::kMaxLine);
  CHECK(ev[1].data["text"] == "xxxxxxxxxx");
}
