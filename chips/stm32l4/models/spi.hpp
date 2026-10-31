#pragma once
// STM32L4 (L496) SPI1-3 in full-duplex master mode, RM0351 section 40 (SPI). Register layout
// and bits follow ST's stm32l496xx.h (SPI_TypeDef, SPI_CR1_*, SPI_CR2_*, SPI_SR_*): CR1 0x00,
// CR2 0x04, SR 0x08, DR 0x0C, CRCPR 0x10, RXCRCR 0x14, TXCRCR 0x18 (read-only). Reset
// values (RM0351 40.6): CR2 = 0x0700 (DS = 8-bit), SR = 0x0002 (TXE), CRCPR = 0x0007.
// Gated by RCC (SPI1 APB2ENR bit 12, SPI2/SPI3 APB1ENR1 bits 14/15: part TOML).
//
// Modelled: CR1 BR/CPOL/CPHA/SPE/LSBFIRST/SSM/SSI/MSTR (CPOL, CPHA, LSBFIRST stored and
// reported; they change neither timing nor the captured frame values), CR2 DS (4-16 bits;
// the reserved values 0-2 are forced to 0111 = 8-bit on write, as on hardware),
// FRXTH/TXEIE/RXNEIE/ERRIE/NSSP (NSSP has no visible effect with software NSS); SR
// RXNE/TXE/OVR/BSY/FRLVL/FTLVL (MODF/FRE/CRCERR/UDR/CHSIDE read 0; a write to SR is accepted
// and changes nothing, CRCERR being rc_w0); DR with the 32-bit TX and RX FIFOs.
//
// FIFOs (RM0351 40.4.9): each frame of DS <= 8 bits takes one byte, wider frames two; both
// FIFOs hold 4 bytes. FTLVL/FRLVL: 0 empty, 1 one byte, 2 two bytes, 3 three or four bytes.
// TXE = TX FIFO <= half full; RXNE = RX FIFO >= 1 byte (FRXTH = 1) or >= 2 bytes
// (FRXTH = 0). DR data packing: with DS <= 8 a 16- or 32-bit access to DR moves two frames
// (low byte first) and an 8-bit access one; with DS > 8 every access moves one frame (a
// 32-bit access is a 16-bit data access: HAL's `hspi->Instance->DR = x`). Reading an empty
// RX FIFO returns 0. Writing DR while SPE = 0 fills the TX FIFO; the transfer starts when SPE
// is set. A write that does not fit in the TX FIFO aborts ("TX FIFO full").
//
// Timing (model contract): a frame starts when SPE = 1 and the TX FIFO is not empty and
// nothing is shifting (its data leaves the TX FIFO then); it takes
// ceil(DS x 2^(BR+1) x 1e9 / PCLK) ns (SPI1: PCLK2, SPI2/3: PCLK1, read from the clock tree
// at each frame start). Its completion is one Clock event, which pushes the MISO frame into
// the RX FIFO (or sets OVR and drops it when it does not fit), starts the next frame if the
// TX FIFO has data and updates the flags and the IRQ line. BSY is set from the first frame's
// start until the completion of a frame with nothing left to send. SPE = 0 lets the frame in
// progress complete and starts no new one. OVR clears on a DR read followed by an SR read.
//
// MISO: a per-bus byte queue filled from outside (call "queue_miso"); each frame takes 1
// byte (DS <= 8) or 2 bytes little-endian (DS > 8), masked to DS bits; missing bytes are
// 0xFF (an empty queue reads all ones). The queue survives SPE toggling, cleared by reset().
//
// Events: one per burst, i.e. per BSY period (frames back to back, BSY never dropping),
// emitted at the instant BSY falls:
//   {"kind":"spi","bus":"SPI1","t_start":ns,"mosi":"<hex>","miso":"<hex>"}
// t_start is the start of the first frame. mosi/miso are lowercase hex of the frame values as
// written to / read from DR (logical values, not wire bit order), one byte per frame for
// DS <= 8, two bytes little-endian per frame for DS > 8. Note that HAL_SPI_TransmitReceive
// alternates TX and RX, so its frames fall into several BSY periods (bursts); use the chip
// select's GPIO history to group bursts into transactions.
//
// IRQ (level, Cortexm::set_irq_level, params.irq): TXEIE & TXE | RXNEIE & RXNE | ERRIE & OVR.
//
// Strict aborts (std::runtime_error "<NAME>: <feature> not modelled (...)"):
//   on any CR1 write: BIDIMODE ("bidirectional mode"), CRCEN/CRCNEXT ("CRC"), RXONLY
//   ("receive-only mode"); on any CR2 write: FRF ("TI mode"), TXDMAEN/RXDMAEN/LDMATX/LDMARX
//   ("DMA"); with SPE = 1 (on the write that sets it or any later CR1/CR2 write): MSTR = 0
//   ("SPI slave not modelled"), SSM = 0 ("hardware NSS"), SSM = 1 with SSI = 0 ("master mode
//   fault"), SSOE = 1 ("hardware NSS output"). Also: a DR write that overflows the TX FIFO,
//   and DR accesses other than at offset 0x0C. The aborting write changes nothing.
//
// params: {"bus": "apb1" | "apb2" (required), "irq": n (optional)}
// call(): "queue_miso" {"data": "<hex>"} -> {"queued": bytes now queued};
//   "state" -> {"enabled", "master", "data_bits", "cpol", "cpha", "lsbfirst", "baud_hz",
//   "frame_ns", "busy", "tx_level" (bytes), "rx_level" (bytes), "ovr", "miso_queued"}.
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "stest/clock.hpp"
#include "stest/model_context.hpp"
#include "stest/peripheral.hpp"

namespace stest::stm32l4 {

class Spi : public Peripheral {
 public:
  Spi(std::string name, const nlohmann::json& params, ModelContext& ctx);
  std::string_view name() const override { return name_; }
  uint32_t read(uint32_t off, unsigned width) override;
  void write(uint32_t off, unsigned width, uint32_t v) override;
  void reset() override;
  void connect(ModelContext& ctx) override;
  nlohmann::json call(std::string_view op, const nlohmann::json& args) override;

 private:
  struct Frame {
    uint16_t v = 0;
    uint8_t bytes = 1;
  };
  struct Fifo {  // at most 4 frames / 4 bytes; fixed storage (no allocation on the MMIO path)
    std::array<Frame, 4> f{};
    unsigned n = 0, bytes = 0;
    void push(Frame x) { f[n++] = x; bytes += x.bytes; }
    Frame pop() {
      Frame x = f[0];
      for (unsigned i = 1; i < n; ++i) f[i - 1] = f[i];
      --n;
      bytes -= x.bytes;
      return x;
    }
    void clear() { n = bytes = 0; }
  };
  [[noreturn]] void not_modelled(const std::string& what, uint32_t off, uint32_t v) const;
  void check_config(uint32_t cr1, uint32_t cr2, uint32_t off, uint32_t v) const;
  unsigned data_bits() const;
  uint64_t pclk_hz() const;
  uint64_t frame_ns() const;
  uint32_t read_dr(unsigned width);
  void write_dr(unsigned width, uint32_t v);
  void start_next();
  void complete();
  uint16_t take_miso(unsigned bytes);
  void sync();  // SR from the FIFO/shift state, then the IRQ level

  std::string name_;
  Clock& clock_;
  Cortexm& core_;
  const ClockTree* clocks_ = nullptr;
  bool apb2_ = false;
  int irq_ = -1;
  bool level_ = false;
  RegisterFile regs_;
  Fifo tx_, rx_;
  bool shifting_ = false, ovr_ = false, dr_read_ = false;
  Frame cur_{};
  EventId ev_ = 0;
  uint64_t burst_start_ = 0;
  std::string mosi_, miso_;  // hex of the current burst
  std::vector<uint8_t> miso_q_;
  size_t miso_head_ = 0;
};

}  // namespace stest::stm32l4
