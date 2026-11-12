// ITM / DWT / CoreDebug models (chips/cortexm) through a Cortexm on a Bus.
#include <doctest/doctest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "stest/cortexm.hpp"
#include "stest/node.hpp"
using namespace stest;

namespace {
constexpr uint64_t ITM = 0xE0000000, DWT = 0xE0001000, DHCSR = 0xE000EDF0, DEMCR = 0xE000EDFC;
uint32_t g_hz = 80'000'000;
uint32_t hz() { return g_hz; }
void nop() {}

struct Rig {
  Bus bus;
  Clock clock;
  IrqHandler table[16];
  std::unique_ptr<Cortexm> core;
  explicit Rig(bool attached) {
    for (auto& h : table) h = &nop;
    g_hz = 80'000'000;
    take_events();
    core = std::make_unique<Cortexm>(bus, clock, 4, table, 16, attached);
    core->set_core_clock_getter(&hz);
  }
  ~Rig() { take_events(); }
  void w(uint64_t a, unsigned width, uint32_t v) { bus.write(a, width, v); }
  uint32_t r(uint64_t a, unsigned width = 4) { return bus.read(a, width); }
  void puts(const std::string& s) { for (char c : s) w(ITM, 1, (unsigned char)c); }
};
}  // namespace

TEST_CASE("attached power-on state: CMSIS ITM_SendChar conditions hold") {
  Rig t(true);
  CHECK((t.r(ITM + 0xE80) & 1u) == 1);
  CHECK((t.r(ITM + 0xE00) & 1u) == 1);
  CHECK((t.r(DEMCR) >> 24 & 1u) == 1);
  CHECK((t.r(DHCSR) & 1u) == 1);
  CHECK(t.r(ITM) == 1);  // FIFOREADY
}

TEST_CASE("detached power-on state: everything off, output dropped") {
  Rig t(false);
  CHECK(t.r(ITM + 0xE80) == 0);
  CHECK(t.r(ITM + 0xE00) == 0);
  CHECK(t.r(DEMCR) == 0);
  CHECK(t.r(DHCSR) == 0);
  t.puts("lost\n");
  CHECK(take_events().empty());
  CHECK(t.r(ITM + 0xFB4) == 3);  // LSR: lock present (SLI) and locked (SLK)
  // firmware enables everything itself, unlocking the ITM first (LAR = 0xC5ACCE55)
  t.w(DEMCR, 4, 1u << 24);
  t.w(ITM + 0xFB0, 4, 0xC5ACCE55u);
  CHECK(t.r(ITM + 0xFB4) == 1);  // unlocked
  t.w(ITM + 0xE80, 4, 1);
  t.w(ITM + 0xE00, 4, 1);
  t.puts("seen\n");
  auto ev = take_events();
  REQUIRE(ev.size() == 1);
  CHECK(ev[0].data["text"] == "seen");
}

TEST_CASE("detached: TER/TPR/TCR writes are ignored while the ITM is locked (no LAR unlock)") {
  Rig t(false);
  t.w(DEMCR, 4, 1u << 24);      // TRCENA
  t.w(ITM + 0xE80, 4, 1);       // TCR.ITMENA: ignored, locked
  t.w(ITM + 0xE00, 4, 1);       // TER port 0: ignored
  t.w(ITM + 0xE40, 4, 0xF);     // TPR: ignored
  CHECK(t.r(ITM + 0xE80) == 0);
  CHECK(t.r(ITM + 0xE00) == 0);
  CHECK(t.r(ITM + 0xE40) == 0);
  CHECK(t.r(ITM + 0xFB4) == 3);
  t.puts("lost\n");
  CHECK(take_events().empty());
  // a wrong key keeps it locked; the right key unlocks; any other value locks again
  t.w(ITM + 0xFB0, 4, 0x12345678u);
  CHECK(t.r(ITM + 0xFB4) == 3);
  t.w(ITM + 0xFB0, 4, 0xC5ACCE55u);
  t.w(ITM + 0xE80, 4, 1);
  t.w(ITM + 0xE00, 4, 1);
  t.puts("ok\n");
  auto ev = take_events();
  REQUIRE(ev.size() == 1);
  CHECK(ev[0].data["text"] == "ok");
  t.w(ITM + 0xFB0, 4, 0);       // lock
  CHECK(t.r(ITM + 0xFB4) == 3);
  t.w(ITM + 0xE80, 4, 0);       // ignored: ITM stays enabled
  CHECK(t.r(ITM + 0xE80) == 1);
}

TEST_CASE("attached: a debugger has unlocked the ITM (LSR = 1)") {
  Rig t(true);
  CHECK(t.r(ITM + 0xFB4) == 1);
  t.w(ITM + 0xE00, 4, 0x3);
  CHECK(t.r(ITM + 0xE00) == 0x3);
}

TEST_CASE("ITM lines: per-port buffers, widths, CRLF, virtual timestamps") {
  Rig t(true);
  t.w(ITM + 0xE00, 4, 0x3);  // ports 0 and 1
  t.clock.advance(1234);
  t.puts("ab");
  t.w(ITM + 4, 1, 'x');
  CHECK(take_events().empty());
  t.w(ITM, 4, 0x0A0D6463u);  // "cd\r\n" as one 32-bit store: one line "abcd"
  t.w(ITM + 4, 2, 0x0A79);   // "y\n"
  auto ev = take_events();
  REQUIRE(ev.size() == 2);
  CHECK(ev[0].t_ns == 1234);
  CHECK(ev[0].data["kind"] == "itm");
  CHECK(ev[0].data["periph"] == "ITM");
  CHECK(ev[0].data["port"] == 0);
  CHECK(ev[0].data["text"] == "abcd");
  CHECK(ev[1].data["port"] == 1);
  CHECK(ev[1].data["text"] == "xy");
  t.puts("tail");
  auto part = t.core->take_itm_partial();
  REQUIRE(part.size() == 1);
  CHECK(part[0].first == 0);
  CHECK(part[0].second == "tail");
  CHECK(t.core->take_itm_partial().empty());
}

TEST_CASE("ITM text with invalid UTF-8 stays valid JSON") {
  Rig t(true);
  t.w(ITM, 1, 0xFF);
  t.w(ITM, 1, '\n');
  auto ev = take_events();
  REQUIRE(ev.size() == 1);
  CHECK_NOTHROW(ev[0].data.dump());
}

TEST_CASE("ITM unmodelled registers abort") {
  Rig t(true);
  CHECK_THROWS_AS(t.r(ITM + 0xFE0), UnmodelledAccess);  // PID0
  CHECK_THROWS_AS(t.r(ITM + 0x100), UnmodelledAccess);
  CHECK_THROWS_AS(t.w(ITM + 0xFB4, 4, 0), UnmodelledAccess);  // LSR is read-only
}

TEST_CASE("DWT CYCCNT: virtual time x core clock, 32-bit wrap, writes re-anchor") {
  Rig t(true);
  CHECK((t.r(DWT) >> 28) == 4);  // NUMCOMP
  t.clock.advance(1000);
  CHECK(t.r(DWT + 4) == 0);  // not enabled
  t.w(DWT, 4, 1);
  CHECK(t.r(DWT + 4) == 0);
  t.clock.advance(1'000'000);  // 1 ms at 80 MHz
  CHECK(t.r(DWT + 4) == 80'000);
  t.w(DWT + 4, 4, 0xFFFFFF00u);
  t.clock.advance(25);  // 2 cycles
  CHECK(t.r(DWT + 4) == 0xFFFFFF02u);
  t.clock.advance(100'000);  // 8000 cycles: wraps
  CHECK(t.r(DWT + 4) == 0xFFFFFF02u + 8000u);
  // stop: value holds
  t.w(DWT, 4, 0);
  uint32_t held = t.r(DWT + 4);
  t.clock.advance(5000);
  CHECK(t.r(DWT + 4) == held);
}

TEST_CASE("DWT: counting needs TRCENA; sub-cycle remainders accumulate") {
  Rig t(false);
  t.w(DWT, 4, 1);  // CYCCNTENA without TRCENA: no counting
  t.clock.advance(1'000'000);
  CHECK(t.r(DWT + 4) == 0);
  t.w(DEMCR, 4, 1u << 24);
  g_hz = 3;  // 3 Hz: one cycle per 333333333.33 ns
  uint32_t n = 0;
  for (int i = 0; i < 3000; ++i) {  // 3000 x 1 ms = 3 s = 9 cycles, read each ms
    t.clock.advance(1'000'000);
    n = t.r(DWT + 4);
  }
  CHECK(n == 9);
}

TEST_CASE("DWT strictness: comparators, other CTRL features, unmodelled offsets") {
  Rig t(true);
  CHECK_NOTHROW(t.w(DWT + 0x20, 4, 0x20000000));  // COMP0
  CHECK_NOTHROW(t.w(DWT + 0x28, 4, 0));           // FUNCTION0 = disabled
  CHECK(t.r(DWT + 0x28) == 0);
  CHECK_THROWS_AS(t.w(DWT + 0x28, 4, 5), std::runtime_error);   // FUNCTION0 enabled
  CHECK_THROWS_AS(t.w(DWT + 0x58, 4, 0xB), std::runtime_error);  // FUNCTION3 enabled
  CHECK_THROWS_AS(t.w(DWT, 4, 1u << 22), std::runtime_error);    // CYCEVTENA
  CHECK_THROWS_AS(t.w(DWT, 4, 1u << 12), std::runtime_error);    // PCSAMPLENA
  CHECK_NOTHROW(t.w(DWT, 4, 0xF0000001u));                     // read-only bits are ignored
  CHECK_THROWS_AS(t.r(DWT + 0x60), UnmodelledAccess);
  CHECK_THROWS_AS(t.w(DWT + 0x1C, 4, 0), UnmodelledAccess);    // PCSR is read-only
}

TEST_CASE("CoreDebug: only TRCENA writable; halting not modelled") {
  Rig t(true);
  CHECK_NOTHROW(t.w(DEMCR, 4, t.r(DEMCR) | (1u << 24)));
  CHECK_THROWS_AS(t.w(DEMCR, 4, 1u << 0), std::runtime_error);  // VC_CORERESET
  t.w(DEMCR, 4, 0);
  CHECK(t.r(DEMCR) == 0);
  CHECK_NOTHROW(t.w(DHCSR, 4, 0xA05F0001u));  // software cannot change C_DEBUGEN
  CHECK((t.r(DHCSR) & 1u) == 1);
  CHECK_THROWS_AS(t.w(DHCSR, 4, 0xA05F0003u), std::runtime_error);  // C_HALT
  CHECK_THROWS_AS(t.r(0xE000EDF4), UnmodelledAccess);  // DCRSR
  CHECK_THROWS_AS(t.r(0xE000EDF8), UnmodelledAccess);  // DCRDR
  Rig d(false);
  CHECK_NOTHROW(d.w(DHCSR, 4, 0xA05F0003u));  // no debugger: ignored
  CHECK(d.r(DHCSR) == 0);
}
