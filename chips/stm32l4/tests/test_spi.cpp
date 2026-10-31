// STM32L4 SPI1 (APB2) and SPI2 (APB1) master on a private Bus + Clock + Cortexm + Rcc
// (80 MHz SYSCLK = HCLK = PCLK1 = PCLK2).
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
#include "stm32l4/models/rcc.hpp"
#include "stm32l4/models/spi.hpp"
using namespace stest;
using nlohmann::json;

namespace {
const IrqHandler kTable[16 + 91] = {};
constexpr uint64_t RCC = 0x40021000, SPI1 = 0x40013000, SPI2 = 0x40003800;
enum : uint32_t { CR1 = 0x00, CR2 = 0x04, SR = 0x08, DR = 0x0C, CRCPR = 0x10, RXCRCR = 0x14, TXCRCR = 0x18 };
// CR1 (SPI_CR1_*)
constexpr uint32_t CPHA = 1u << 0, CPOL = 1u << 1, MSTR = 1u << 2, SPE = 1u << 6, LSBFIRST = 1u << 7,
                   SSI = 1u << 8, SSM = 1u << 9, RXONLY = 1u << 10, CRCEN = 1u << 13,
                   BIDIMODE = 1u << 15;
constexpr uint32_t BR_DIV16 = 3u << 3, BR_DIV2 = 0u << 3;
// CR2 (SPI_CR2_*)
constexpr uint32_t RXDMAEN = 1u << 0, TXDMAEN = 1u << 1, SSOE = 1u << 2, FRF = 1u << 4,
                   ERRIE = 1u << 5, RXNEIE = 1u << 6, TXEIE = 1u << 7, FRXTH = 1u << 12;
constexpr uint32_t DS8 = 7u << 8, DS16 = 15u << 8, DS4 = 3u << 8;
// SR (SPI_SR_*)
constexpr uint32_t RXNE = 1u << 0, TXE = 1u << 1, OVR = 1u << 6, BSY = 1u << 7;
constexpr uint32_t FRLVL_SHIFT = 9, FTLVL_SHIFT = 11;
constexpr int IRQ_SPI1 = 35, IRQ_SPI2 = 36;
constexpr uint64_t kNone = UINT64_MAX;
constexpr uint32_t MASTER = MSTR | SSM | SSI;  // HAL: SPI_MODE_MASTER + SPI_NSS_SOFT

struct Rig {
  Bus bus;
  Clock clock;
  Cortexm core{bus, clock, 4, kTable, 16 + 91};
  ModelContext ctx{clock, core, bus};
  stm32l4::Rcc rcc{"RCC", json::object(), ctx};
  stm32l4::Spi spi1{"SPI1", {{"bus", "apb2"}, {"irq", IRQ_SPI1}}, ctx};
  stm32l4::Spi spi2{"SPI2", {{"bus", "apb1"}, {"irq", IRQ_SPI2}}, ctx};
  Rig() {
    for (Peripheral* p : std::vector<Peripheral*>{&rcc, &spi1, &spi2}) {
      p->reset();
      ctx.add(p);
    }
    ctx.set_clock_tree(&rcc);
    for (Peripheral* p : ctx.models()) p->connect(ctx);
    bus.map(RCC, 0x400, &rcc);
    bus.map(SPI1, 0x400, &spi1);
    bus.map(SPI2, 0x400, &spi2);
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
  void to(uint64_t t) {
    while (clock.next_event_ns() <= t) {
      clock.advance(clock.next_event_ns() - clock.now_ns());
      clock.run_due();
    }
    clock.advance(t - clock.now_ns());
  }
  uint64_t fire_next() {
    uint64_t t = clock.next_event_ns();
    if (t == kNone) return kNone;
    to(t);
    return t;
  }
  // Master mode 0, /16, soft NSS, enabled; CR2 as given (default 8-bit, FRXTH = 1).
  void master(uint64_t base = SPI1, uint32_t cr2 = DS8 | FRXTH, uint32_t br = BR_DIV16) {
    wr(base + CR1, MASTER | br);
    wr(base + CR2, cr2);
    wr(base + CR1, MASTER | br | SPE);
  }
};

std::vector<json> spi_events() {
  std::vector<json> out;
  for (const Event& e : take_events())
    if (e.data.value("kind", "") == "spi") out.push_back(json{{"t", e.t_ns}, {"e", e.data}});
  return out;
}

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

TEST_CASE("spi reset values and register map") {
  Rig r;
  CHECK(r.rd(SPI1 + CR1) == 0);
  CHECK(r.rd(SPI1 + CR2) == 0x0700u);  // DS = 8-bit
  CHECK(r.rd(SPI1 + SR) == TXE);
  CHECK(r.rd(SPI1 + CRCPR) == 0x0007u);
  CHECK(r.rd(SPI1 + RXCRCR) == 0);
  CHECK(r.rd(SPI1 + TXCRCR) == 0);
  CHECK_THROWS_AS(r.rd(SPI1 + 0x1C), UnmodelledAccess);       // I2SCFGR: no I2S on L4 SPI
  CHECK_THROWS_AS(r.wr(SPI1 + RXCRCR, 1), UnmodelledAccess);  // read-only
  CHECK_THROWS_AS(r.wr(SPI1 + TXCRCR, 1), UnmodelledAccess);
  // CR1 / CR2 / CRCPR masks; reserved DS values are forced to 8-bit (RM0351 40.6.2)
  r.wr(SPI1 + CR1, 0xFFFF0000u | MASTER);
  CHECK(r.rd(SPI1 + CR1) == MASTER);
  r.wr(SPI1 + CR2, 0x1u << 8);
  CHECK(r.rd(SPI1 + CR2) == DS8);
  r.wr(SPI1 + CR2, 0xFFFF0000u | DS4);
  CHECK(r.rd(SPI1 + CR2) == DS4);
  r.wr(SPI1 + CRCPR, 0xFFFF1234u);
  CHECK(r.rd(SPI1 + CRCPR) == 0x1234u);
  CHECK(r.clock.next_event_ns() == kNone);
}

TEST_CASE("spi frame timing: DS bits x 2^(BR+1) / PCLK") {
  Rig r;
  r.master();
  r.wr(SPI1 + DR, 0xA5, 1);
  CHECK((r.rd(SPI1 + SR) & BSY) != 0);
  CHECK(r.clock.next_event_ns() == 1600);  // 8 bits at 80 MHz / 16
  CHECK(r.fire_next() == 1600);
  const uint32_t sr = r.rd(SPI1 + SR);
  CHECK((sr & BSY) == 0);
  CHECK((sr & RXNE) != 0);
  CHECK(r.rd(SPI1 + DR, 1) == 0xFF);  // MISO default: all ones
  CHECK(r.clock.next_event_ns() == kNone);

  // SPI2 is on APB1: PPRE1 = /2 (40 MHz) doubles the frame time; /2 prescaler, 16-bit frames
  r.wr(RCC + 0x08, r.rd(RCC + 0x08) | (4u << 8));
  r.master(SPI2, DS16, BR_DIV2);
  const uint64_t t0 = r.clock.now_ns();
  r.wr(SPI2 + DR, 0x1234, 2);
  CHECK(r.clock.next_event_ns() == t0 + 800);  // 16 x 2 / 40 MHz
  // ...and changing the clock tree between frames is picked up at the next frame start
  r.fire_next();
  r.wr(RCC + 0x08, r.rd(RCC + 0x08) & ~(7u << 8));
  r.wr(SPI2 + DR, 0x5678, 2);
  CHECK(r.clock.next_event_ns() == r.clock.now_ns() + 400);
}

TEST_CASE("spi FIFO levels, TXE and RXNE with FRXTH") {
  Rig r;
  r.master(SPI1, DS8);  // FRXTH = 0: RXNE needs 16 bits in the RX FIFO
  r.wr(SPI1 + DR, 0x0201, 2);  // packing: two 8-bit frames, low byte first
  r.wr(SPI1 + DR, 0x0403, 2);
  // frame 0x01 is shifting; three bytes queued -> FTLVL full, TXE clear
  uint32_t sr = r.rd(SPI1 + SR);
  CHECK(((sr >> FTLVL_SHIFT) & 3) == 3);
  CHECK((sr & TXE) == 0);
  CHECK((sr & BSY) != 0);
  CHECK(r.fire_next() == 1600);
  sr = r.rd(SPI1 + SR);
  CHECK(((sr >> FTLVL_SHIFT) & 3) == 2);  // half: TXE set
  CHECK((sr & TXE) != 0);
  CHECK(((sr >> FRLVL_SHIFT) & 3) == 1);
  CHECK((sr & RXNE) == 0);                // 8 bits < 16-bit threshold
  r.fire_next();
  sr = r.rd(SPI1 + SR);
  CHECK(((sr >> FRLVL_SHIFT) & 3) == 2);
  CHECK((sr & RXNE) != 0);
  // FRXTH = 1: one byte is enough
  r.wr(SPI1 + CR2, DS8 | FRXTH);
  CHECK((r.rd(SPI1 + SR) & RXNE) != 0);
  CHECK(r.rd(SPI1 + DR, 2) == 0xFFFF);  // 16-bit read pops two 8-bit frames
  sr = r.rd(SPI1 + SR);
  CHECK((sr & RXNE) == 0);
  CHECK(((sr >> FRLVL_SHIFT) & 3) == 0);
  r.fire_next();
  r.fire_next();
  sr = r.rd(SPI1 + SR);
  CHECK((sr & BSY) == 0);
  CHECK(((sr >> FTLVL_SHIFT) & 3) == 0);
  CHECK(((sr >> FRLVL_SHIFT) & 3) == 2);
  CHECK(r.rd(SPI1 + DR, 1) == 0xFF);  // 8-bit read pops one frame
  CHECK(((r.rd(SPI1 + SR) >> FRLVL_SHIFT) & 3) == 1);
  // writing a full TX FIFO is a strict abort
  r.wr(SPI1 + CR1, MASTER | BR_DIV16);  // SPE = 0: the TX FIFO fills, nothing shifts
  r.wr(SPI1 + DR, 0x0201, 2);
  r.wr(SPI1 + DR, 0x0403, 2);
  CHECK(r.clock.next_event_ns() == kNone);
  CHECK((r.rd(SPI1 + SR) & BSY) == 0);
  CHECK(abort_message([&] { r.wr(SPI1 + DR, 5, 1); }).find("TX FIFO full") != std::string::npos);
}

TEST_CASE("spi DR written while SPE = 0 is sent when SPE is set") {
  Rig r;
  r.wr(SPI1 + CR1, MASTER | BR_DIV16);
  r.wr(SPI1 + CR2, DS8 | FRXTH);
  r.wr(SPI1 + DR, 0x42, 1);
  CHECK(r.clock.next_event_ns() == kNone);
  r.to(1000);
  r.wr(SPI1 + CR1, MASTER | BR_DIV16 | SPE);
  CHECK(r.clock.next_event_ns() == 2600);
  r.fire_next();
  auto ev = spi_events();
  REQUIRE(ev.size() == 1);
  CHECK(ev[0]["e"]["mosi"] == "42");
  CHECK(ev[0]["e"]["t_start"] == 1000);
}

TEST_CASE("spi OVR when the RX FIFO overflows; cleared by DR then SR read") {
  Rig r;
  r.master(SPI1, DS8 | FRXTH | ERRIE);
  r.spi1.call("queue_miso", {{"data", "0102030405"}});
  for (uint32_t b = 0; b < 5; ++b) {
    r.wr(SPI1 + DR, b, 1);
    r.fire_next();
  }
  uint32_t sr = r.rd(SPI1 + SR);
  CHECK((sr & OVR) != 0);
  CHECK(((sr >> FRLVL_SHIFT) & 3) == 3);
  CHECK(r.core.pending(IRQ_SPI1));  // ERRIE
  // the frame that overflowed is lost: the FIFO keeps the first four
  CHECK(r.rd(SPI1 + DR, 2) == 0x0201);
  CHECK((r.rd(SPI1 + SR) & OVR) != 0);  // this read returns OVR still set, then clears it
  CHECK((r.rd(SPI1 + SR) & OVR) == 0);
  CHECK(r.rd(SPI1 + DR, 2) == 0x0403);
  CHECK(r.rd(SPI1 + DR, 2) == 0);  // empty RX FIFO reads 0
}

TEST_CASE("spi burst grouping: one event per BSY period, when BSY falls") {
  Rig r;
  r.master();
  r.spi1.call("queue_miso", {{"data", "1020"}});
  r.to(100);
  r.wr(SPI1 + DR, 0x01A5, 2);  // A5, 01
  r.wr(SPI1 + DR, 0x0302, 2);  // 02, 03: refilled before the FIFO drains -> BSY stays high
  for (int i = 0; i < 3; ++i) {
    r.fire_next();
    CHECK(spi_events().empty());
    CHECK((r.rd(SPI1 + SR) & BSY) != 0);
  }
  CHECK(r.fire_next() == 100 + 4 * 1600);
  auto ev = spi_events();
  REQUIRE(ev.size() == 1);
  CHECK(ev[0]["t"] == 100 + 4 * 1600);
  CHECK(ev[0]["e"] == json{{"kind", "spi"}, {"periph", "SPI1"}, {"t_start", 100},
                           {"mosi", "a5010203"}, {"miso", "1020ffff"}});
  CHECK(r.clock.next_event_ns() == kNone);
  // a frame started after BSY fell is a new burst
  r.wr(SPI1 + DR, 0x77, 1);
  r.fire_next();
  ev = spi_events();
  REQUIRE(ev.size() == 1);
  CHECK(ev[0]["e"]["mosi"] == "77");
  CHECK(ev[0]["e"]["t_start"] == 100 + 4 * 1600);
}

TEST_CASE("spi frames wider than 8 bits are little-endian bytes; narrower are masked") {
  Rig r;
  r.master(SPI1, DS16);
  r.spi1.call("queue_miso", {{"data", "cdab"}});
  r.wr(SPI1 + DR, 0x1234, 2);
  r.wr(SPI1 + DR, 0x5678, 4);  // 32-bit store (HAL: hspi->Instance->DR = ...) = 16-bit data
  r.fire_next();
  r.fire_next();
  auto ev = spi_events();
  REQUIRE(ev.size() == 1);
  CHECK(ev[0]["e"]["mosi"] == "34127856");
  CHECK(ev[0]["e"]["miso"] == "cdabffff");
  CHECK(r.rd(SPI1 + DR, 2) == 0xABCD);
  CHECK(r.rd(SPI1 + DR, 2) == 0xFFFF);
  r.wr(SPI1 + CR1, MASTER | BR_DIV16);
  r.wr(SPI1 + CR2, DS4 | FRXTH);
  r.wr(SPI1 + CR1, MASTER | BR_DIV16 | SPE);
  r.wr(SPI1 + DR, 0xFF, 1);
  CHECK(r.clock.next_event_ns() == r.clock.now_ns() + 800);  // 4 bits x 16 / 80 MHz
  r.fire_next();
  ev = spi_events();
  REQUIRE(ev.size() == 1);
  CHECK(ev[0]["e"]["mosi"] == "0f");
  CHECK(ev[0]["e"]["miso"] == "0f");
  CHECK(r.rd(SPI1 + DR, 1) == 0x0F);
}

TEST_CASE("spi IRQ levels: TXEIE, RXNEIE") {
  Rig r;
  r.master();
  CHECK_FALSE(r.core.pending(IRQ_SPI1));
  r.wr(SPI1 + CR2, DS8 | FRXTH | TXEIE);  // TXE is set: the line goes high at once
  CHECK(r.core.pending(IRQ_SPI1));
  r.core.clear_pending(IRQ_SPI1);  // the line is still high: re-pends (ARMv7-M)
  CHECK(r.core.pending(IRQ_SPI1));
  r.wr(SPI1 + CR2, DS8 | FRXTH | RXNEIE);  // TXEIE off: the line drops
  r.core.clear_pending(IRQ_SPI1);
  CHECK_FALSE(r.core.pending(IRQ_SPI1));
  r.wr(SPI1 + DR, 1, 1);
  CHECK_FALSE(r.core.pending(IRQ_SPI1));
  r.fire_next();
  CHECK(r.core.pending(IRQ_SPI1));
  (void)r.rd(SPI1 + DR, 1);  // RXNE falls
  r.core.clear_pending(IRQ_SPI1);
  CHECK_FALSE(r.core.pending(IRQ_SPI1));
  r.wr(SPI1 + DR, 1, 1);
  r.fire_next();
  CHECK(r.core.pending(IRQ_SPI1));
}

TEST_CASE("spi not-modelled features abort") {
  Rig r;
  auto msg = [&](uint64_t a, uint32_t v) { return abort_message([&] { r.wr(a, v); }); };
  // stored while disabled (HAL's __HAL_SPI_DISABLE / CR1 = 0 is fine)...
  r.wr(SPI1 + CR1, 0);
  r.wr(SPI1 + CR1, MSTR | SSI);  // hardware NSS while SPE = 0 is fine too
  // ...but enabling unmodelled behaviour aborts
  CHECK(msg(SPI1 + CR1, SSM | SSI | SPE) == "SPI1: SPI slave mode (CR1.MSTR=0 with SPE=1) not modelled");
  CHECK_THROWS_AS(r.wr(SPI1 + CR1, SSM | SSI | SPE), NotModelled);
  CHECK(msg(SPI1 + CR1, MSTR | SPE).find("hardware NSS") != std::string::npos);
  CHECK(msg(SPI1 + CR1, MSTR | SSM | SPE).find("mode fault") != std::string::npos);
  CHECK(msg(SPI1 + CR1, MASTER | BIDIMODE).find("bidirectional") != std::string::npos);
  CHECK(msg(SPI1 + CR1, MASTER | CRCEN).find("CRC") != std::string::npos);
  CHECK(msg(SPI1 + CR1, MASTER | RXONLY).find("receive-only") != std::string::npos);
  CHECK(msg(SPI1 + CR2, DS8 | FRF).find("TI mode") != std::string::npos);
  CHECK(msg(SPI1 + CR2, DS8 | TXDMAEN).find("DMA") != std::string::npos);
  CHECK(msg(SPI1 + CR2, DS8 | RXDMAEN).find("DMA") != std::string::npos);
  r.wr(SPI1 + CR1, MASTER);
  r.wr(SPI1 + CR2, DS8 | SSOE);  // stored while disabled
  CHECK(msg(SPI1 + CR1, MASTER | SPE).find("NSS output") != std::string::npos);
  // the aborted writes changed nothing
  CHECK(r.rd(SPI1 + CR1) == MASTER);
  CHECK(r.rd(SPI1 + CR2) == (DS8 | SSOE));
  CHECK(r.clock.next_event_ns() == kNone);
  // CPOL/CPHA/LSBFIRST are accepted (no effect on timing or captured values)
  r.wr(SPI1 + CR2, DS8 | FRXTH);
  r.wr(SPI1 + CR1, MASTER | CPOL | CPHA | LSBFIRST | SPE);
  CHECK(r.spi1.call("state", json::object())["cpol"] == 1);
}

TEST_CASE("spi call ops: queue_miso, state") {
  Rig r;
  CHECK_THROWS_AS(r.spi1.call("queue_miso", {{"data", "abc"}}), std::invalid_argument);
  CHECK_THROWS_AS(r.spi1.call("queue_miso", {{"data", "zz"}}), std::invalid_argument);
  CHECK_THROWS_AS(r.spi1.call("queue_miso", json::object()), std::invalid_argument);
  CHECK_THROWS_AS(r.spi1.call("bogus", json::object()), UnknownOp);
  r.spi1.call("queue_miso", {{"data", "AB01"}});
  r.master();
  json st = r.spi1.call("state", json::object());
  CHECK(st["enabled"] == true);
  CHECK(st["master"] == true);
  CHECK(st["data_bits"] == 8);
  CHECK(st["baud_hz"] == 5000000.0);
  CHECK(st["frame_ns"] == 1600);
  CHECK(st["miso_queued"] == 2);
  CHECK(st["busy"] == false);
  // the queue is not cleared by SPE toggling but by reset
  r.spi1.reset();
  CHECK(r.spi1.call("state", json::object())["miso_queued"] == 0);
}

TEST_CASE("spi drives its IRQ through set_irq_source (safe on a shared vector)") {
  Rig r;
  r.master();
  int other = 0;
  r.ctx.set_irq_source(IRQ_SPI1, &other, true);
  r.wr(SPI1 + CR2, DS8 | FRXTH | TXEIE);  // SPI's source rises...
  r.wr(SPI1 + CR2, DS8 | FRXTH);          // ...and drops; the other keeps the line high
  r.core.clear_pending(IRQ_SPI1);
  CHECK(r.core.pending(IRQ_SPI1));
  r.ctx.set_irq_source(IRQ_SPI1, &other, false);
  r.core.clear_pending(IRQ_SPI1);
  CHECK_FALSE(r.core.pending(IRQ_SPI1));
}
