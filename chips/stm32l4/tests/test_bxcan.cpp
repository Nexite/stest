// STM32L4 bxCAN (CAN1 master + CAN2 slave) on a private Bus + Clock + Cortexm + Rcc
// (80 MHz SYSCLK = HCLK = PCLK1). The CAN bus engine is emulated by calling deliver().
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include "stest/bus.hpp"
#include "stest/clock.hpp"
#include "stest/cortexm.hpp"
#include "stest/model_context.hpp"
#include "stest/node.hpp"
#include "stm32l4/models/bxcan.hpp"
#include "stm32l4/models/rcc.hpp"
using namespace stest;
using nlohmann::json;

namespace {
const IrqHandler kTable[16 + 91] = {};
constexpr uint64_t RCC = 0x40021000, CAN1 = 0x40006400, CAN2 = 0x40006800;
enum : uint32_t {
  MCR = 0x00, MSR = 0x04, TSR = 0x08, RF0R = 0x0C, RF1R = 0x10, IER = 0x14, ESR = 0x18, BTR = 0x1C,
  TI0R = 0x180, TDT0R = 0x184, TDL0R = 0x188, TDH0R = 0x18C,
  RI0R = 0x1B0, RDT0R = 0x1B4, RDL0R = 0x1B8, RDH0R = 0x1BC,
  FMR = 0x200, FM1R = 0x204, FS1R = 0x20C, FFA1R = 0x214, FA1R = 0x21C, F0R1 = 0x240,
};
// MCR (CAN_MCR_*)
constexpr uint32_t INRQ = 1u << 0, SLEEP = 1u << 1, TXFP = 1u << 2, RFLM = 1u << 3, NART = 1u << 4,
                   AWUM = 1u << 5, TTCM = 1u << 7, RESET = 1u << 15, DBF = 1u << 16;
// MSR (CAN_MSR_*)
constexpr uint32_t INAK = 1u << 0, SLAK = 1u << 1, ERRI = 1u << 2, SLAKI = 1u << 4;
// TSR (CAN_TSR_*), mailbox m at << 8m
constexpr uint32_t RQCP = 1u << 0, TXOK = 1u << 1, ALST = 1u << 2, TERR = 1u << 3, ABRQ = 1u << 7;
constexpr uint32_t CODE_POS = 24, TME0 = 1u << 26, LOW0 = 1u << 29;
// RFxR
constexpr uint32_t FMP = 3u, FULL = 1u << 3, FOVR = 1u << 4, RFOM = 1u << 5;
// IER
constexpr uint32_t TMEIE = 1u << 0, FMPIE0 = 1u << 1, FFIE0 = 1u << 2, FOVIE0 = 1u << 3,
                   FMPIE1 = 1u << 4, LECIE = 1u << 11, ERRIE = 1u << 15, WKUIE = 1u << 16,
                   SLKIE = 1u << 17;
// BTR: 500 kbit/s at PCLK1 = 80 MHz: BRP = 9, TS1 = 12 (13 tq), TS2 = 1 (2 tq) -> 16 tq x 125 ns
constexpr uint32_t BTR_500K = 9u | (12u << 16) | (1u << 20), LBKM = 1u << 30, SILM = 1u << 31;
constexpr uint64_t BIT_NS = 2000;
// TIxR
constexpr uint32_t TXRQ = 1u << 0, RTR = 1u << 1, IDE = 1u << 2;
constexpr int IRQ_TX = 19, IRQ_RX0 = 20, IRQ_RX1 = 21, IRQ_SCE = 22;
constexpr int IRQ2_TX = 86, IRQ2_RX0 = 87, IRQ2_RX1 = 88, IRQ2_SCE = 89;
constexpr uint64_t kNone = UINT64_MAX;

uint32_t std_tir(uint32_t id) { return id << 21; }
uint32_t ext_tir(uint32_t id) { return (id << 3) | IDE; }

struct Rig {
  Bus bus;
  Clock clock;
  Cortexm core{bus, clock, 4, kTable, 16 + 91};
  ModelContext ctx{clock, core, bus};
  stm32l4::Rcc rcc{"RCC", json::object(), ctx};
  stm32l4::Bxcan can1{"CAN1", {{"role", "master"}, {"irq", {{"tx", IRQ_TX}, {"rx0", IRQ_RX0}, {"rx1", IRQ_RX1}, {"sce", IRQ_SCE}}}}, ctx};
  stm32l4::Bxcan can2{"CAN2", {{"role", "slave"}, {"master", "CAN1"}, {"irq", {{"tx", IRQ2_TX}, {"rx0", IRQ2_RX0}, {"rx1", IRQ2_RX1}, {"sce", IRQ2_SCE}}}}, ctx};
  Rig() {
    for (Peripheral* p : std::vector<Peripheral*>{&rcc, &can1, &can2}) {
      p->reset();
      ctx.add(p);
    }
    ctx.set_clock_tree(&rcc);
    for (Peripheral* p : ctx.models()) p->connect(ctx);
    bus.map(RCC, 0x400, &rcc);
    bus.map(CAN1, 0x400, &can1);
    bus.map(CAN2, 0x400, &can2);
    // HSE 8 MHz bypass, PLL M=2 N=40 R=2 -> SYSCLK = HCLK = PCLK1 = PCLK2 = 80 MHz
    rcc_set(0x00, (1u << 18) | (1u << 16));
    bus.write(RCC + 0x0C, 4, 3u | (1u << 4) | (40u << 8) | (1u << 24));
    rcc_set(0x00, 1u << 24);
    bus.write(RCC + 0x08, 4, 3u);
    (void)take_events();
  }
  void rcc_set(uint32_t off, uint32_t bits) { bus.write(RCC + off, 4, bus.read(RCC + off, 4) | bits); }
  uint32_t rd(uint64_t a, unsigned w = 4) { return bus.read(a, w); }
  void wr(uint64_t a, uint32_t v, unsigned w = 4) { bus.write(a, w, v); }
  void set(uint64_t a, uint32_t bits) { wr(a, rd(a) | bits); }
  void clr(uint64_t a, uint32_t bits) { wr(a, rd(a) & ~bits); }
  void to(uint64_t t) { clock.advance_to(t); }
  uint64_t fire_next() {
    uint64_t t = clock.next_event_ns();
    if (t == kNone) return kNone;
    to(t);
    return t;
  }
  bool irq_high(int n) {  // level input: clear_pending re-pends while the line is high
    core.clear_pending(n);
    return core.pending(n);
  }
  // HAL_CAN_Init (INRQ, wait INAK, clear SLEEP, MCR options, BTR) + HAL_CAN_Start.
  void init(uint64_t base, uint32_t mcr_opts = 0, uint32_t btr = BTR_500K) {
    set(base + MCR, INRQ);
    REQUIRE((rd(base + MSR) & INAK) != 0);
    clr(base + MCR, SLEEP);
    REQUIRE((rd(base + MSR) & SLAK) == 0);
    set(base + MCR, mcr_opts);
    wr(base + BTR, btr);
  }
  void start(uint64_t base) {
    clr(base + MCR, INRQ);
    REQUIRE(fire_next() != kNone);  // 11 recessive bits
    REQUIRE((rd(base + MSR) & INAK) == 0);
  }
  void up(uint64_t base, uint32_t mcr_opts = 0) {
    init(base, mcr_opts);
    start(base);
  }
  // HAL_CAN_AddTxMessage on mailbox m.
  void load(uint64_t base, unsigned m, uint32_t tir, uint32_t dlc, uint32_t dl = 0, uint32_t dh = 0) {
    const uint64_t mb = base + TI0R + 0x10 * m;
    wr(mb, tir);
    wr(mb + 4, dlc);
    wr(mb + 0xC, dh);
    wr(mb + 8, dl);
  }
  void send(uint64_t base, unsigned m, uint32_t tir, uint32_t dlc, uint32_t dl = 0, uint32_t dh = 0) {
    load(base, m, tir, dlc, dl, dh);
    set(base + TI0R + 0x10 * m, TXRQ);
  }
  // Accept-everything filter on bank b for FIFO f (32-bit mask, mask 0).
  void accept_all(unsigned b, unsigned f = 0) {
    set(CAN1 + FMR, 1);
    clr(CAN1 + FA1R, 1u << b);
    set(CAN1 + FS1R, 1u << b);
    clr(CAN1 + FM1R, 1u << b);
    if (f) set(CAN1 + FFA1R, 1u << b);
    else clr(CAN1 + FFA1R, 1u << b);
    wr(CAN1 + F0R1 + 8 * b, 0);
    wr(CAN1 + F0R1 + 8 * b + 4, 0);
    set(CAN1 + FA1R, 1u << b);
    clr(CAN1 + FMR, 1);
  }
};

std::vector<json> can_events() {
  std::vector<json> out;
  for (const Event& e : take_events()) {
    const std::string k = e.data.value("kind", "");
    if (k.rfind("can_", 0) == 0) {
      json j = e.data;
      j["t"] = e.t_ns;
      out.push_back(j);
    }
  }
  return out;
}

json rx(uint32_t id, std::string data, bool ext = false, bool rtr = false, int dlc = -1) {
  return {{"id", id}, {"ext", ext}, {"rtr", rtr}, {"dlc", dlc < 0 ? static_cast<int>(data.size() / 2) : dlc}, {"data", data}};
}
json ok(unsigned m, uint64_t seq) { return {{"mailbox", m}, {"seq", seq}, {"ok", true}, {"arb_lost", false}, {"error", nullptr}}; }

template <class F>
std::string abort_message(F&& f) {
  try {
    f();
  } catch (const std::exception& e) {
    return e.what();
  }
  return "(no exception)";
}
}  // namespace

TEST_CASE("bxcan reset values and register map") {
  Rig r;
  CHECK(r.rd(CAN1 + MCR) == 0x00010002u);
  CHECK(r.rd(CAN1 + MSR) == 0x00000C02u);
  CHECK(r.rd(CAN1 + TSR) == 0x1C000000u);
  CHECK(r.rd(CAN1 + RF0R) == 0);
  CHECK(r.rd(CAN1 + RF1R) == 0);
  CHECK(r.rd(CAN1 + IER) == 0);
  CHECK(r.rd(CAN1 + ESR) == 0);
  CHECK(r.rd(CAN1 + BTR) == 0x01230000u);
  CHECK(r.rd(CAN1 + FMR) == 0x2A1C0E01u);
  CHECK(r.rd(CAN2 + MCR) == 0x00010002u);
  CHECK(r.can1.mode() == stm32l4::Bxcan::Mode::Sleep);
  // filter registers live in CAN1's block only
  CHECK_THROWS_AS(r.rd(CAN2 + FMR), UnmodelledAccess);
  CHECK_THROWS_AS(r.wr(CAN2 + F0R1, 0), UnmodelledAccess);
  CHECK_THROWS_AS(r.rd(CAN1 + 0x20), UnmodelledAccess);  // reserved
  CHECK_THROWS_AS(r.rd(CAN1 + 0x208), UnmodelledAccess);
  // RX FIFO mailboxes are read-only
  CHECK_THROWS_AS(r.wr(CAN1 + RI0R, 0), UnmodelledAccess);
  CHECK(r.clock.next_event_ns() == kNone);
  CHECK(can_events().empty());
}

TEST_CASE("bxcan init and normal transitions: INAK clears after 11 recessive bits") {
  Rig r;
  r.to(1000);
  r.set(CAN1 + MCR, INRQ);  // from sleep: INRQ wins (HAL_CAN_Init order)
  CHECK((r.rd(CAN1 + MSR) & (INAK | SLAK)) == INAK);
  CHECK(r.can1.mode() == stm32l4::Bxcan::Mode::Init);
  auto ev = can_events();
  REQUIRE(ev.size() == 1);
  CHECK(ev[0] == json{{"kind", "can_mode"}, {"periph", "CAN1"}, {"mode", "init"}, {"bitrate", nullptr}, {"t", 1000}});
  r.clr(CAN1 + MCR, SLEEP);
  CHECK((r.rd(CAN1 + MSR) & (INAK | SLAK)) == INAK);
  CHECK(can_events().empty());  // still init
  r.wr(CAN1 + BTR, BTR_500K);
  CHECK(r.rd(CAN1 + BTR) == BTR_500K);
  CHECK(r.can1.bitrate() == 500000);
  r.to(5000);
  r.clr(CAN1 + MCR, INRQ);
  CHECK((r.rd(CAN1 + MSR) & INAK) != 0);  // waiting for 11 recessive bits
  CHECK(r.clock.next_event_ns() == 5000 + 11 * BIT_NS);
  r.to(5000 + 11 * BIT_NS - 1);
  CHECK((r.rd(CAN1 + MSR) & INAK) != 0);
  r.to(5000 + 11 * BIT_NS);
  CHECK((r.rd(CAN1 + MSR) & (INAK | SLAK)) == 0);
  CHECK(r.can1.mode() == stm32l4::Bxcan::Mode::Normal);
  ev = can_events();
  REQUIRE(ev.size() == 1);
  CHECK(ev[0] == json{{"kind", "can_mode"}, {"periph", "CAN1"}, {"mode", "normal"}, {"bitrate", 500000}, {"t", 27000}});
  // back to init: immediate; sleep from normal: immediate with SLAKI when SLKIE
  r.set(CAN1 + MCR, INRQ);
  CHECK((r.rd(CAN1 + MSR) & INAK) != 0);
  CHECK(can_events().at(0)["mode"] == "init");
  // INRQ set and cleared again before synchronising: the pending sync is cancelled
  r.clr(CAN1 + MCR, INRQ);
  r.set(CAN1 + MCR, INRQ);
  CHECK(r.clock.next_event_ns() == kNone);
  CHECK(can_events().empty());
  r.start(CAN1);
  (void)can_events();
  r.set(CAN1 + IER, SLKIE);
  r.set(CAN1 + MCR, SLEEP);
  CHECK((r.rd(CAN1 + MSR) & (INAK | SLAK | SLAKI)) == (SLAK | SLAKI));
  CHECK(r.irq_high(IRQ_SCE));
  CHECK(can_events().at(0)["mode"] == "sleep");
  r.wr(CAN1 + MSR, SLAKI);  // rc_w1
  CHECK((r.rd(CAN1 + MSR) & SLAKI) == 0);
  CHECK_FALSE(r.irq_high(IRQ_SCE));
  // wake up: SLAK clears after 11 bits
  r.clr(CAN1 + MCR, SLEEP);
  CHECK((r.rd(CAN1 + MSR) & SLAK) != 0);
  r.fire_next();
  CHECK((r.rd(CAN1 + MSR) & SLAK) == 0);
  CHECK(can_events().at(0)["mode"] == "normal");
  // CAN2 has its own mode
  CHECK(r.can2.mode() == stm32l4::Bxcan::Mode::Sleep);
}

TEST_CASE("bxcan BTR: bitrate, write-protected outside init, LBKM/SILM strict") {
  Rig r;
  // write outside initialization mode (reset: sleep) aborts
  CHECK(abort_message([&] { r.wr(CAN1 + BTR, BTR_500K); }).find("BTR") != std::string::npos);
  CHECK(r.rd(CAN1 + BTR) == 0x01230000u);
  r.init(CAN1);
  CHECK(r.can1.bitrate() == 500000);
  r.wr(CAN1 + BTR, 4u | (12u << 16) | (1u << 20));  // BRP=4 -> 1 Mbit/s
  CHECK(r.can1.bitrate() == 1000000);
  CHECK_THROWS_AS(r.wr(CAN1 + BTR, BTR_500K | LBKM), NotModelled);
  CHECK_THROWS_AS(r.wr(CAN1 + BTR, BTR_500K | SILM), NotModelled);
  r.wr(CAN1 + BTR, BTR_500K);
  r.start(CAN1);
  CHECK_THROWS_AS(r.wr(CAN1 + BTR, BTR_500K), NotModelled);
}

TEST_CASE("bxcan strict: TTCM, AWUM, WKUIE") {
  Rig r;
  CHECK_THROWS_AS(r.set(CAN1 + MCR, TTCM), NotModelled);
  CHECK_THROWS_AS(r.set(CAN1 + MCR, AWUM), NotModelled);
  CHECK_THROWS_AS(r.set(CAN1 + IER, WKUIE), NotModelled);
  CHECK(r.rd(CAN1 + MCR) == 0x00010002u);  // the aborting write changed nothing
  CHECK(r.rd(CAN1 + IER) == 0);
}

TEST_CASE("bxcan TX: request event, mailbox busy until tx_result, TX IRQ") {
  Rig r;
  r.up(CAN1);
  r.set(CAN1 + IER, TMEIE);
  (void)can_events();
  const uint64_t t0 = r.clock.now_ns();
  r.send(CAN1, 0, std_tir(0x123), 3, 0x00CCBBAAu);
  auto ev = can_events();
  REQUIRE(ev.size() == 1);
  CHECK(ev[0] == json{{"kind", "can_tx"}, {"periph", "CAN1"}, {"mailbox", 0}, {"id", 0x123}, {"ext", false},
                      {"rtr", false}, {"dlc", 3}, {"data", "aabbcc"}, {"txfp", 0}, {"nart", false},
                      {"seq", 1}, {"t", t0}});
  uint32_t tsr = r.rd(CAN1 + TSR);
  CHECK((tsr & TME0) == 0);
  CHECK((tsr & (TME0 << 1)) != 0);
  CHECK(((tsr >> CODE_POS) & 3) == 1);  // next free mailbox
  CHECK((r.rd(CAN1 + TI0R) & TXRQ) != 0);
  // the mailbox registers are write-protected while pending
  CHECK_THROWS_AS(r.wr(CAN1 + TI0R, std_tir(0x7)), NotModelled);
  CHECK_THROWS_AS(r.wr(CAN1 + TDL0R, 0), NotModelled);
  CHECK_FALSE(r.irq_high(IRQ_TX));
  // the engine's result: applied at t, not before
  const uint64_t eof = t0 + 100000;
  r.can1.deliver("tx_result", ok(0, 1), eof);
  CHECK((r.rd(CAN1 + TSR) & TME0) == 0);
  r.to(eof - 1);
  CHECK((r.rd(CAN1 + TSR) & TME0) == 0);
  r.to(eof);
  tsr = r.rd(CAN1 + TSR);
  CHECK((tsr & (RQCP | TXOK | ALST | TERR)) == (RQCP | TXOK));
  CHECK((tsr & TME0) != 0);
  CHECK((r.rd(CAN1 + TI0R) & TXRQ) == 0);
  CHECK(r.irq_high(IRQ_TX));
  r.wr(CAN1 + TSR, RQCP);  // __HAL_CAN_CLEAR_FLAG(RQCP0): clears RQCP/TXOK/ALST/TERR
  CHECK((r.rd(CAN1 + TSR) & 0xFF) == 0);
  CHECK_FALSE(r.irq_high(IRQ_TX));
  // extended remote frame on mailbox 0 again; seq increases
  r.send(CAN1, 0, ext_tir(0x1ABCDEF0) | RTR, 2);
  ev = can_events();
  REQUIRE(ev.size() == 1);
  CHECK(ev[0]["id"] == 0x1ABCDEF0);
  CHECK(ev[0]["ext"] == true);
  CHECK(ev[0]["rtr"] == true);
  CHECK(ev[0]["dlc"] == 2);
  CHECK(ev[0]["data"] == "");
  CHECK(ev[0]["seq"] == 2);
  // DLC > 8 is not modelled
  CHECK_THROWS_AS(r.send(CAN1, 1, std_tir(1), 9), NotModelled);
}

TEST_CASE("bxcan TX: TXRQ outside normal mode is held and requested on entering normal") {
  Rig r;
  r.init(CAN1);
  (void)can_events();
  r.send(CAN1, 2, std_tir(0x300), 0);
  r.send(CAN1, 0, std_tir(0x100), 1, 0x11);
  CHECK(can_events().empty());
  CHECK((r.rd(CAN1 + TSR) & (5u << 26)) == 0);  // TME0, TME2 clear
  CHECK(r.can1.call("state", json::object())["tx"][2]["state"] == "held");
  r.start(CAN1);
  auto ev = can_events();
  REQUIRE(ev.size() == 3);
  CHECK(ev[0]["mode"] == "normal");
  CHECK(ev[1]["mailbox"] == 2);  // TXRQ order
  CHECK(ev[1]["seq"] == 1);
  CHECK(ev[2]["mailbox"] == 0);
  CHECK(ev[2]["seq"] == 2);
  CHECK(ev[2]["data"] == "11");
  CHECK(ev[1]["t"] == ev[0]["t"]);
}

TEST_CASE("bxcan TX: TXFP, CODE and LOW") {
  Rig r;
  r.up(CAN1);
  (void)can_events();
  r.send(CAN1, 0, std_tir(0x200), 0);
  r.send(CAN1, 1, std_tir(0x400), 0);
  uint32_t tsr = r.rd(CAN1 + TSR);
  CHECK(((tsr >> CODE_POS) & 3) == 2);
  CHECK((tsr & (7u << 29)) == (LOW0 << 1));  // mailbox 1 has the highest ID
  r.send(CAN1, 2, std_tir(0x100), 0);
  tsr = r.rd(CAN1 + TSR);
  CHECK((tsr & (7u << 26)) == 0);
  CHECK(((tsr >> CODE_POS) & 3) == 1);  // all busy: the lowest-priority mailbox
  CHECK((tsr & (7u << 29)) == (LOW0 << 1));
  for (const json& e : can_events()) CHECK(e["txfp"] == 0);
  // complete all, then TXFP = 1: priority by request order
  for (unsigned m = 0; m < 3; ++m) r.can1.deliver("tx_result", ok(m, m + 1), r.clock.now_ns() + 10);
  r.to(r.clock.now_ns() + 10);
  r.wr(CAN1 + TSR, RQCP | (RQCP << 8) | (RQCP << 16));
  r.set(CAN1 + MCR, INRQ);
  r.set(CAN1 + MCR, TXFP);
  r.start(CAN1);
  (void)can_events();
  r.send(CAN1, 1, std_tir(0x100), 0);
  r.send(CAN1, 0, std_tir(0x400), 0);
  r.send(CAN1, 2, std_tir(0x200), 0);
  tsr = r.rd(CAN1 + TSR);
  CHECK(((tsr >> CODE_POS) & 3) == 2);  // the latest request
  CHECK((tsr & (7u << 29)) == (LOW0 << 2));
  auto ev = can_events();
  REQUIRE(ev.size() == 3);
  for (const json& e : ev) CHECK(e["txfp"] == 1);
  CHECK(ev[0]["seq"] == 4);
}

TEST_CASE("bxcan TX: ABRQ, stale results, held abort") {
  Rig r;
  r.up(CAN1);
  r.set(CAN1 + IER, TMEIE);
  (void)can_events();
  r.send(CAN1, 1, std_tir(0x55), 0);
  CHECK(can_events().at(0)["seq"] == 1);
  r.to(r.clock.now_ns() + 500);
  const uint64_t ta = r.clock.now_ns();
  r.set(CAN1 + TSR, ABRQ << 8);  // HAL_CAN_AbortTxRequest
  auto ev = can_events();
  REQUIRE(ev.size() == 1);
  CHECK(ev[0] == json{{"kind", "can_tx_abort"}, {"periph", "CAN1"}, {"mailbox", 1}, {"seq", 1}, {"t", ta}});
  uint32_t tsr = r.rd(CAN1 + TSR);
  CHECK((tsr & (TME0 << 1)) == 0);  // busy until the engine answers
  CHECK((tsr & (ABRQ << 8)) != 0);
  r.set(CAN1 + TSR, ABRQ << 8);  // a second ABRQ: no new event
  CHECK(can_events().empty());
  // stale (wrong seq) result: ignored
  r.can1.deliver("tx_result", ok(1, 7), ta + 10);
  r.to(ta + 10);
  CHECK((r.rd(CAN1 + TSR) & (TME0 << 1)) == 0);
  // effective abort
  r.can1.deliver("tx_result", {{"mailbox", 1}, {"seq", 1}, {"ok", false}, {"arb_lost", false}, {"error", nullptr}, {"aborted", true}}, ta + 88000);
  r.to(ta + 88000);
  tsr = r.rd(CAN1 + TSR);
  CHECK((tsr & (0xFFu << 8)) == (RQCP << 8));  // RQCP=1, TXOK=0, ABRQ cleared
  CHECK((tsr & (TME0 << 1)) != 0);
  CHECK(r.irq_high(IRQ_TX));
  r.wr(CAN1 + TSR, RQCP << 8);
  // late result for the aborted request: ignored
  r.can1.deliver("tx_result", ok(1, 1), ta + 90000);
  r.to(ta + 90000);
  CHECK((r.rd(CAN1 + TSR) & (0xFFu << 8)) == 0);
  // an abort that came too late (after SOF): the normal result completes the mailbox
  r.send(CAN1, 0, std_tir(0x56), 0);
  r.set(CAN1 + TSR, ABRQ);
  CHECK(can_events().size() == 2);
  r.can1.deliver("tx_result", ok(0, 2), r.clock.now_ns() + 5);
  r.fire_next();
  CHECK((r.rd(CAN1 + TSR) & 0xFF) == (RQCP | TXOK));
  r.wr(CAN1 + TSR, RQCP);
  // ABRQ on an empty mailbox: no effect
  r.set(CAN1 + TSR, ABRQ << 16);
  CHECK((r.rd(CAN1 + TSR) & (0xFFu << 16)) == 0);
  CHECK(can_events().empty());
  // ABRQ on a held mailbox (init mode): completes at once, no event
  r.set(CAN1 + MCR, INRQ);
  (void)can_events();
  r.send(CAN1, 2, std_tir(0x57), 0);
  r.set(CAN1 + TSR, ABRQ << 16);
  CHECK((r.rd(CAN1 + TSR) & (0xFFu << 16)) == (RQCP << 16));
  CHECK((r.rd(CAN1 + TSR) & (TME0 << 2)) != 0);
  CHECK(can_events().empty());
}

TEST_CASE("bxcan TX: arbitration lost / ACK error with and without NART") {
  Rig r;
  r.up(CAN1);
  r.wr(CAN1 + IER, ERRIE | LECIE);
  (void)can_events();
  r.send(CAN1, 0, std_tir(0x10), 0);
  const uint64_t t = r.clock.now_ns();
  r.can1.deliver("tx_result", {{"mailbox", 0}, {"seq", 1}, {"ok", false}, {"arb_lost", true}, {"error", nullptr}}, t + 100);
  r.to(t + 100);
  uint32_t tsr = r.rd(CAN1 + TSR);
  CHECK((tsr & 0xFF) == ALST);  // NART = 0: still pending, retried by the engine
  CHECK((tsr & TME0) == 0);
  r.can1.deliver("tx_result", {{"mailbox", 0}, {"seq", 1}, {"ok", false}, {"arb_lost", false}, {"error", "ack"}}, t + 200);
  r.to(t + 200);
  tsr = r.rd(CAN1 + TSR);
  CHECK((tsr & 0xFF) == TERR);
  CHECK((tsr & TME0) == 0);
  CHECK(((r.rd(CAN1 + ESR) >> 4) & 7) == 3);  // LEC = ACK error
  CHECK((r.rd(CAN1 + MSR) & ERRI) != 0);
  CHECK(r.irq_high(IRQ_SCE));
  r.clr(CAN1 + ESR, 0x70);  // HAL: CLEAR_BIT(ESR, LEC)
  r.wr(CAN1 + MSR, ERRI);
  CHECK_FALSE(r.irq_high(IRQ_SCE));
  r.can1.deliver("tx_result", ok(0, 1), t + 300);
  r.to(t + 300);
  CHECK((r.rd(CAN1 + TSR) & 0xFF) == (RQCP | TXOK));
  r.wr(CAN1 + TSR, RQCP);
  // NART = 1: one attempt; a failure completes the mailbox
  r.set(CAN1 + MCR, INRQ);
  r.set(CAN1 + MCR, NART);
  r.start(CAN1);
  (void)can_events();
  r.send(CAN1, 0, std_tir(0x10), 0);
  auto ev = can_events();
  CHECK(ev.at(0)["nart"] == true);
  r.can1.deliver("tx_result", {{"mailbox", 0}, {"seq", 2}, {"ok", false}, {"arb_lost", false}, {"error", "ack"}}, r.clock.now_ns() + 100);
  r.fire_next();
  tsr = r.rd(CAN1 + TSR);
  CHECK((tsr & 0xFF) == (RQCP | TERR));
  CHECK((tsr & TME0) != 0);
  r.wr(CAN1 + TSR, RQCP);
  r.send(CAN1, 0, std_tir(0x10), 0);
  r.can1.deliver("tx_result", {{"mailbox", 0}, {"seq", 3}, {"ok", false}, {"arb_lost", true}, {"error", nullptr}}, r.clock.now_ns() + 100);
  r.fire_next();
  CHECK((r.rd(CAN1 + TSR) & 0xFF) == (RQCP | ALST));
}

TEST_CASE("bxcan TX: leaving normal withdraws requests; re-requested on return") {
  Rig r;
  r.up(CAN1);
  (void)can_events();
  r.send(CAN1, 0, std_tir(0x10), 0);
  r.send(CAN1, 1, std_tir(0x11), 0);
  (void)can_events();
  const uint64_t t1 = r.clock.now_ns();
  r.set(CAN1 + MCR, INRQ);
  auto ev = can_events();
  REQUIRE(ev.size() == 3);
  CHECK(ev[0]["kind"] == "can_mode");
  CHECK(ev[1] == json{{"kind", "can_tx_abort"}, {"periph", "CAN1"}, {"mailbox", 0}, {"seq", 1}, {"t", t1}});
  CHECK(ev[2]["mailbox"] == 1);
  // mailbox 0's attempt was already on the wire: completes normally
  r.can1.deliver("tx_result", ok(0, 1), t1 + 50);
  // mailbox 1 withdrawn: back to held (still busy, no RQCP)
  r.can1.deliver("tx_result", {{"mailbox", 1}, {"seq", 2}, {"ok", false}, {"aborted", true}}, t1 + 88000);
  r.to(t1 + 88000);
  uint32_t tsr = r.rd(CAN1 + TSR);
  CHECK((tsr & 0xFF) == (RQCP | TXOK));
  CHECK((tsr & (0xFFu << 8)) == 0);
  CHECK((tsr & (TME0 << 1)) == 0);
  CHECK(r.can1.call("state", json::object())["tx"][1]["state"] == "held");
  r.start(CAN1);
  ev = can_events();
  REQUIRE(ev.size() == 2);
  CHECK(ev[1]["kind"] == "can_tx");
  CHECK(ev[1]["mailbox"] == 1);
  CHECK(ev[1]["seq"] == 3);
}

TEST_CASE("bxcan RX: 32-bit mask filter, FIFO 0, output mailbox, RFOM") {
  Rig r;
  // HAL_CAN_ConfigFilter: bank 0, 32-bit mask, ID 0x123 std, mask all ID bits + IDE + RTR
  r.set(CAN1 + FMR, 1);
  r.clr(CAN1 + FA1R, 1);
  r.set(CAN1 + FS1R, 1);
  r.wr(CAN1 + F0R1, std_tir(0x123));
  r.wr(CAN1 + F0R1 + 4, std_tir(0x7FF) | IDE | RTR);
  r.clr(CAN1 + FM1R, 1);
  r.clr(CAN1 + FFA1R, 1);
  r.set(CAN1 + FA1R, 1);
  r.clr(CAN1 + FMR, 1);
  r.up(CAN1);
  r.wr(CAN1 + IER, FMPIE0);
  const uint64_t t = r.clock.now_ns() + 1000;
  r.can1.deliver("rx", rx(0x123, "0102030405"), t);
  CHECK((r.rd(CAN1 + RF0R) & FMP) == 0);  // not before t
  r.to(t);
  CHECK((r.rd(CAN1 + RF0R) & FMP) == 1);
  CHECK(r.irq_high(IRQ_RX0));
  CHECK(r.rd(CAN1 + RI0R) == std_tir(0x123));
  CHECK(r.rd(CAN1 + RDT0R) == 5);  // DLC 5, FMI 0, TIME 0
  CHECK(r.rd(CAN1 + RDL0R) == 0x04030201u);
  CHECK(r.rd(CAN1 + RDH0R) == 0x05u);
  // non-matching: different ID, extended, remote
  r.can1.deliver("rx", rx(0x124, ""), t + 10);
  r.can1.deliver("rx", rx(0x123, "", true), t + 20);
  r.can1.deliver("rx", rx(0x123, "", false, true, 0), t + 30);
  r.to(t + 30);
  CHECK((r.rd(CAN1 + RF0R) & FMP) == 1);
  // CAN2 did not get it (separate controller, sleeping)
  CHECK((r.rd(CAN2 + RF0R) & FMP) == 0);
  r.set(CAN1 + RF0R, RFOM);
  CHECK((r.rd(CAN1 + RF0R) & (FMP | RFOM)) == 0);
  CHECK_FALSE(r.irq_high(IRQ_RX0));
  r.set(CAN1 + RF0R, RFOM);  // empty: no effect
  CHECK((r.rd(CAN1 + RF0R) & FMP) == 0);
}

TEST_CASE("bxcan RX: not received outside normal mode or while FINIT") {
  Rig r;
  r.accept_all(0);
  r.init(CAN1);
  r.can1.deliver("rx", rx(0x1, "aa"), r.clock.now_ns() + 10);
  r.fire_next();
  CHECK((r.rd(CAN1 + RF0R) & FMP) == 0);
  r.start(CAN1);
  r.set(CAN1 + FMR, 1);
  r.can1.deliver("rx", rx(0x1, "aa"), r.clock.now_ns() + 10);
  r.fire_next();
  CHECK((r.rd(CAN1 + RF0R) & FMP) == 0);
  r.clr(CAN1 + FMR, 1);
  r.can1.deliver("rx", rx(0x1, "aa"), r.clock.now_ns() + 10);
  r.fire_next();
  CHECK((r.rd(CAN1 + RF0R) & FMP) == 1);
}

TEST_CASE("bxcan filters: 16-bit list, FIFO1 assignment, FMI numbering across banks") {
  Rig r;
  r.set(CAN1 + FMR, 1);
  // bank 0: 32-bit mask, FIFO 0, inactive              -> FIFO0 filter 0
  // bank 1: 16-bit list, FIFO 1, IDs 0x10 0x11 0x12 0x13 -> FIFO1 filters 0..3
  // bank 2: 16-bit mask, FIFO 0, (0x20,0x7FF) (0x30, 0x7F0) -> FIFO0 filters 1, 2
  // bank 3: 32-bit list, FIFO 1, ext 0x1234567 and std 0x40 -> FIFO1 filters 4, 5
  r.wr(CAN1 + FS1R, (1u << 0) | (1u << 3));
  r.wr(CAN1 + FM1R, (1u << 1) | (1u << 3));
  r.wr(CAN1 + FFA1R, (1u << 1) | (1u << 3));
  auto h = [](uint32_t id) { return id << 5; };  // 16-bit std: STID at [15:5]
  r.wr(CAN1 + F0R1 + 8 * 1, h(0x10) | (h(0x11) << 16));
  r.wr(CAN1 + F0R1 + 8 * 1 + 4, h(0x12) | (h(0x13) << 16));
  r.wr(CAN1 + F0R1 + 8 * 2, h(0x20) | ((h(0x7FF) | 0x18) << 16));  // mask incl. RTR/IDE
  r.wr(CAN1 + F0R1 + 8 * 2 + 4, h(0x30) | ((h(0x7F0) | 0x18) << 16));
  r.wr(CAN1 + F0R1 + 8 * 3, ext_tir(0x1234567));
  r.wr(CAN1 + F0R1 + 8 * 3 + 4, std_tir(0x40));
  r.wr(CAN1 + FA1R, 0xE);  // banks 1-3 active
  r.clr(CAN1 + FMR, 1);
  r.up(CAN1);
  struct C { uint32_t id; bool ext; unsigned fifo; uint32_t fmi; };
  const C cases[] = {{0x12, false, 1, 2}, {0x13, false, 1, 3}, {0x20, false, 0, 1}, {0x3A, false, 0, 2},
                     {0x1234567, true, 1, 4}, {0x40, false, 1, 5}, {0x10, false, 1, 0}};
  for (const C& c : cases) {
    CAPTURE(c.id);
    r.can1.deliver("rx", rx(c.id, "", c.ext), r.clock.now_ns() + 10);
    r.fire_next();
    const uint64_t rfr = CAN1 + (c.fifo ? RF1R : RF0R);
    REQUIRE((r.rd(rfr) & FMP) == 1);
    CHECK(r.rd(CAN1 + RI0R + 0x10 * c.fifo) == (c.ext ? ext_tir(c.id) : std_tir(c.id)));
    CHECK(((r.rd(CAN1 + RDT0R + 0x10 * c.fifo) >> 8) & 0xFF) == c.fmi);
    r.set(rfr, RFOM);
  }
  // 16-bit list does not match 0x14; 16-bit mask 0x30/0x7F0 does not match 0x40 std via bank 2
  r.can1.deliver("rx", rx(0x14, ""), r.clock.now_ns() + 10);
  r.fire_next();
  CHECK((r.rd(CAN1 + RF0R) & FMP) == 0);
  CHECK((r.rd(CAN1 + RF1R) & FMP) == 0);
  // filter writes need FINIT (or an inactive bank for FiRx)
  CHECK_THROWS_AS(r.wr(CAN1 + FM1R, 0), NotModelled);
  CHECK_THROWS_AS(r.wr(CAN1 + F0R1 + 8, 0), NotModelled);
  r.wr(CAN1 + F0R1, 0);  // bank 0 inactive: allowed
}

TEST_CASE("bxcan filters: 32-bit beats 16-bit, list beats mask") {
  Rig r;
  r.set(CAN1 + FMR, 1);
  // bank 0: 16-bit mask accept-all (FIFO 0); bank 1: 32-bit mask accept-all (FIFO 1);
  // bank 2: 32-bit list for 0x77 (FIFO 0)
  r.wr(CAN1 + FS1R, 0x6);
  r.wr(CAN1 + FM1R, 0x4);
  r.wr(CAN1 + FFA1R, 0x2);
  r.wr(CAN1 + F0R1, 0);
  r.wr(CAN1 + F0R1 + 4, 0);
  r.wr(CAN1 + F0R1 + 8, 0);
  r.wr(CAN1 + F0R1 + 12, 0);
  r.wr(CAN1 + F0R1 + 16, std_tir(0x77));
  r.wr(CAN1 + F0R1 + 20, std_tir(0x77));
  r.wr(CAN1 + FA1R, 0x7);
  r.clr(CAN1 + FMR, 1);
  r.up(CAN1);
  r.can1.deliver("rx", rx(0x76, ""), r.clock.now_ns() + 10);
  r.fire_next();
  CHECK((r.rd(CAN1 + RF1R) & FMP) == 1);  // 32-bit mask over 16-bit mask
  r.can1.deliver("rx", rx(0x77, ""), r.clock.now_ns() + 10);
  r.fire_next();
  CHECK((r.rd(CAN1 + RF0R) & FMP) == 1);  // 32-bit list over 32-bit mask
  CHECK(((r.rd(CAN1 + RDT0R) >> 8) & 0xFF) == 2);  // FIFO0: bank 0 has 2 filters, bank 2 -> 2
}

TEST_CASE("bxcan filters: CAN2SB splits the banks between CAN1 and CAN2") {
  Rig r;
  r.set(CAN1 + FMR, 1);
  r.wr(CAN1 + FMR, (r.rd(CAN1 + FMR) & ~(0x3Fu << 8)) | (20u << 8));  // CAN2SB = 20
  r.clr(CAN1 + FMR, 1);
  r.accept_all(0, 0);   // CAN1
  r.accept_all(21, 1);  // CAN2, FIFO 1
  r.accept_all(20, 0);  // CAN2, FIFO 0 -> FMI 0 on CAN2
  r.up(CAN1);
  r.up(CAN2);
  r.can1.deliver("rx", rx(0x5, "01"), r.clock.now_ns() + 10);
  r.can2.deliver("rx", rx(0x6, "02"), r.clock.now_ns() + 10);
  r.fire_next();
  CHECK((r.rd(CAN1 + RF0R) & FMP) == 1);
  CHECK((r.rd(CAN1 + RF1R) & FMP) == 0);
  CHECK((r.rd(CAN2 + RF0R) & FMP) == 1);  // bank 20 (lower bank wins over bank 21)
  CHECK(r.rd(CAN2 + RI0R) == std_tir(0x6));
  CHECK(((r.rd(CAN2 + RDT0R) >> 8) & 0xFF) == 0);
  // CAN2SB = 28: CAN2 has no filters
  r.set(CAN1 + FMR, 1);
  r.wr(CAN1 + FMR, (r.rd(CAN1 + FMR) & ~(0x3Fu << 8)) | (28u << 8));
  r.clr(CAN1 + FMR, 1);
  r.can2.deliver("rx", rx(0x6, "02"), r.clock.now_ns() + 10);
  r.fire_next();
  CHECK((r.rd(CAN2 + RF0R) & FMP) == 1);
  CHECK((r.rd(CAN2 + RF1R) & FMP) == 0);
  r.set(CAN1 + FMR, 1);
  CHECK_THROWS_AS(r.wr(CAN1 + FMR, 1u | (29u << 8)), NotModelled);
}

TEST_CASE("bxcan RX FIFO: FULL, overrun with RFLM=0 (overwrite) and RFLM=1 (lock), IRQs") {
  for (bool lock : {false, true}) {
    CAPTURE(lock);
    Rig r;
    r.accept_all(0);
    r.up(CAN1, lock ? RFLM : 0);
    r.wr(CAN1 + IER, FFIE0);
    for (unsigned i = 0; i < 3; ++i) {
      r.can1.deliver("rx", rx(0x100 + i, "0" + std::to_string(i)), r.clock.now_ns() + 10);
      r.fire_next();
    }
    CHECK((r.rd(CAN1 + RF0R) & (FMP | FULL | FOVR)) == (3u | FULL));
    CHECK(r.irq_high(IRQ_RX0));
    r.wr(CAN1 + IER, FOVIE0);
    CHECK_FALSE(r.irq_high(IRQ_RX0));
    r.can1.deliver("rx", rx(0x200, "ff"), r.clock.now_ns() + 10);
    r.fire_next();
    CHECK((r.rd(CAN1 + RF0R) & (FMP | FULL | FOVR)) == (3u | FULL | FOVR));
    CHECK(r.irq_high(IRQ_RX0));
    // read out: 0x100, 0x101, then 0x102 (locked) or 0x200 (overwritten)
    CHECK(r.rd(CAN1 + RI0R) == std_tir(0x100));
    r.set(CAN1 + RF0R, RFOM);  // HAL's SET_BIT also clears FULL/FOVR (rc_w1 read back as 1)
    CHECK((r.rd(CAN1 + RF0R) & (FMP | FULL | FOVR)) == 2u);
    CHECK_FALSE(r.irq_high(IRQ_RX0));
    CHECK(r.rd(CAN1 + RI0R) == std_tir(0x101));
    r.wr(CAN1 + RF0R, RFOM);
    CHECK(r.rd(CAN1 + RI0R) == std_tir(lock ? 0x102 : 0x200));
    CHECK(r.rd(CAN1 + RDL0R) == (lock ? 0x02u : 0xFFu));
    r.wr(CAN1 + RF0R, RFOM);
    CHECK((r.rd(CAN1 + RF0R) & FMP) == 0);
  }
}

TEST_CASE("bxcan RX FIFO1 IRQ and FIFO0/1 independence") {
  Rig r;
  r.accept_all(0, 1);
  r.up(CAN1);
  r.wr(CAN1 + IER, FMPIE1 | FMPIE0);
  r.can1.deliver("rx", rx(0x1, ""), r.clock.now_ns() + 10);
  r.fire_next();
  CHECK(r.irq_high(IRQ_RX1));
  CHECK_FALSE(r.irq_high(IRQ_RX0));
  CHECK((r.rd(CAN1 + RF1R) & FMP) == 1);
}

TEST_CASE("bxcan deliver: validation, unknown op, no immediate effect") {
  Rig r;
  CHECK_THROWS_AS(r.can1.deliver("bogus", json::object(), 0), UnknownOp);
  CHECK_THROWS_AS(r.can1.deliver("rx", {{"id", 1}}, 0), std::invalid_argument);
  CHECK_THROWS_AS(r.can1.deliver("rx", rx(0x800, ""), 0), std::invalid_argument);  // std id range
  CHECK_THROWS_AS(r.can1.deliver("rx", rx(0x1, "aabb", false, false, 3), 0), std::invalid_argument);
  CHECK_THROWS_AS(r.can1.deliver("rx", rx(0x1, "", false, false, 9), 0), std::invalid_argument);
  CHECK_THROWS_AS(r.can1.deliver("rx", rx(0x1, "zz"), 0), std::invalid_argument);
  CHECK_THROWS_AS(r.can1.deliver("tx_result", {{"mailbox", 3}, {"seq", 1}, {"ok", true}}, 0), std::invalid_argument);
  CHECK_THROWS_AS(r.can1.deliver("tx_result", {{"mailbox", 0}, {"ok", true}}, 0), std::invalid_argument);
  CHECK_THROWS_AS(r.can1.deliver("tx_result", {{"mailbox", 0}, {"seq", 1}, {"ok", false}}, 0), std::invalid_argument);
  CHECK_THROWS_AS(r.can1.deliver("tx_result", {{"mailbox", 0}, {"seq", 1}, {"ok", false}, {"error", "zap"}}, 0), std::invalid_argument);
  CHECK(r.clock.next_event_ns() == kNone);
}

TEST_CASE("bxcan state call, MCR.RESET, clock change re-reports the bitrate") {
  Rig r;
  r.up(CAN1);
  r.send(CAN1, 0, std_tir(0x1), 0);
  json s = r.can1.call("state", json::object());
  CHECK(s["mode"] == "normal");
  CHECK(s["bitrate"] == 500000);
  CHECK(s["tec"] == 0);
  CHECK(s["rec"] == 0);
  CHECK(s["fifo"] == json::array({0, 0}));
  CHECK(s["tx"][0] == json{{"state", "pending"}, {"seq", 1}, {"abort", false}});
  CHECK(s["tx"][1] == json{{"state", "empty"}, {"seq", nullptr}, {"abort", false}});
  CHECK_THROWS_AS(r.can1.call("nope", json::object()), UnknownOp);
  (void)can_events();
  // APB1 prescaler /2 -> PCLK1 = 40 MHz -> 250 kbit/s
  r.wr(RCC + 0x08, r.rd(RCC + 0x08) | (4u << 8));
  auto ev = can_events();
  REQUIRE(ev.size() == 1);
  CHECK(ev[0]["mode"] == "normal");
  CHECK(ev[0]["bitrate"] == 250000);
  // master reset: withdraw, back to sleep with reset registers
  r.set(CAN1 + MCR, RESET);
  ev = can_events();
  REQUIRE(ev.size() == 2);
  CHECK(ev[0]["kind"] == "can_tx_abort");
  CHECK(ev[1]["mode"] == "sleep");
  CHECK(r.rd(CAN1 + MCR) == 0x00010002u);
  CHECK(r.rd(CAN1 + MSR) == 0x00000C02u);
  CHECK(r.rd(CAN1 + TSR) == 0x1C000000u);
  CHECK(r.rd(CAN1 + BTR) == 0x01230000u);
  r.can1.deliver("tx_result", ok(0, 1), r.clock.now_ns() + 5);  // stale now
  r.fire_next();
  CHECK(r.rd(CAN1 + TSR) == 0x1C000000u);
}
