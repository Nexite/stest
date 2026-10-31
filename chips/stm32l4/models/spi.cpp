#include "stm32l4/models/spi.hpp"
#include <cstdio>
#include <stdexcept>
#include "stest/cortexm.hpp"
#include "stest/node.hpp"

namespace stest::stm32l4 {
namespace {
enum : uint32_t { CR1 = 0x00, CR2 = 0x04, SR = 0x08, DR = 0x0C, CRCPR = 0x10, RXCRCR = 0x14, TXCRCR = 0x18 };
// CR1 (SPI_CR1_*)
constexpr uint32_t CPHA = 1u << 0, CPOL = 1u << 1, MSTR = 1u << 2, BR_POS = 3, BR = 7u << 3,
                   SPE = 1u << 6, LSBFIRST = 1u << 7, SSI = 1u << 8, SSM = 1u << 9,
                   RXONLY = 1u << 10, CRCNEXT = 1u << 12, CRCEN = 1u << 13, BIDIMODE = 1u << 15;
// CR2 (SPI_CR2_*)
constexpr uint32_t RXDMAEN = 1u << 0, TXDMAEN = 1u << 1, SSOE = 1u << 2, FRF = 1u << 4,
                   ERRIE = 1u << 5, RXNEIE = 1u << 6, TXEIE = 1u << 7, DS_POS = 8, DS = 0xFu << 8,
                   FRXTH = 1u << 12, LDMARX = 1u << 13, LDMATX = 1u << 14;
// SR (SPI_SR_*)
constexpr uint32_t RXNE = 1u << 0, TXE = 1u << 1, CRCERR = 1u << 4, OVR = 1u << 6, BSY = 1u << 7,
                   FRLVL_POS = 9, FTLVL_POS = 11;
constexpr unsigned kFifoBytes = 4;

std::string hex32(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08x", v);
  return b;
}
uint32_t level_code(unsigned bytes) { return bytes >= 3 ? 3u : bytes; }
void append_hex(std::string& s, uint16_t v, unsigned bytes) {
  static const char* d = "0123456789abcdef";
  for (unsigned i = 0; i < bytes; ++i) {
    const unsigned b = (v >> (8 * i)) & 0xFFu;
    s.push_back(d[b >> 4]);
    s.push_back(d[b & 0xF]);
  }
}
int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace

Spi::Spi(std::string name, const nlohmann::json& params, ModelContext& ctx)
    : name_(std::move(name)), clock_(ctx.clock()), core_(ctx.core()) {
  const std::string bus = params.value("bus", "");
  if (bus != "apb1" && bus != "apb2")
    throw std::invalid_argument(name_ + ": spi params.bus must be \"apb1\" or \"apb2\"");
  apb2_ = bus == "apb2";
  irq_ = params.value("irq", -1);
  regs_.declare(CR1, 0, 0xFFFFu);
  regs_.declare(CR2, 0x0700u, 0x7FFFu);
  regs_.declare(SR, TXE, CRCERR);  // only CRCERR (rc_w0) is writable; it never sets
  regs_.declare(CRCPR, 0x0007u, 0xFFFFu);
  regs_.declare(RXCRCR, 0, 0);
  regs_.declare(TXCRCR, 0, 0);
  mosi_.reserve(64);
  miso_.reserve(64);
}

void Spi::connect(ModelContext& ctx) { clocks_ = &ctx.clocks(); }

void Spi::reset() {
  if (shifting_) clock_.cancel(ev_);
  shifting_ = ovr_ = dr_read_ = false;
  tx_.clear();
  rx_.clear();
  mosi_.clear();
  miso_.clear();
  miso_q_.clear();
  miso_head_ = 0;
  regs_.reset();
  sync();
}

void Spi::not_modelled(const std::string& what) const { throw NotModelled(name_, what); }

void Spi::check_config(uint32_t cr1, uint32_t cr2, uint32_t off, uint32_t v) const {
  if (cr1 & BIDIMODE) not_modelled("bidirectional mode (CR1.BIDIMODE)");
  if (cr1 & (CRCEN | CRCNEXT)) not_modelled("CRC (CR1.CRCEN/CRCNEXT)");
  if (cr1 & RXONLY) not_modelled("receive-only mode (CR1.RXONLY)");
  if (cr2 & FRF) not_modelled("TI mode (CR2.FRF)");
  if (cr2 & (TXDMAEN | RXDMAEN | LDMATX | LDMARX))
    not_modelled("DMA (CR2.TXDMAEN/RXDMAEN/LDMATX/LDMARX)");
  if (!(cr1 & SPE)) return;
  if (!(cr1 & MSTR)) not_modelled("SPI slave mode (CR1.MSTR=0 with SPE=1)");
  if (!(cr1 & SSM)) not_modelled("hardware NSS (CR1.SSM=0; use software NSS: SSM=1, SSI=1)");
  if (!(cr1 & SSI)) not_modelled("master mode fault (CR1.SSM=1, SSI=0 -> MODF)");
  if (cr2 & SSOE) not_modelled("hardware NSS output (CR2.SSOE=1)");
}

unsigned Spi::data_bits() const { return ((regs_.get(CR2) & DS) >> DS_POS) + 1; }

uint64_t Spi::pclk_hz() const {
  if (!clocks_) throw std::logic_error(name_ + ": not connected to a clock tree");
  return apb2_ ? clocks_->pclk2_hz() : clocks_->pclk1_hz();
}

uint64_t Spi::frame_ns() const {
  const uint64_t pclk = pclk_hz();
  if (pclk == 0) throw std::runtime_error(name_ + ": APB clock is 0 Hz");
  const uint64_t div = 2ull << ((regs_.get(CR1) & BR) >> BR_POS);
  const uint64_t num = data_bits() * div * 1000000000ull;
  return (num + pclk - 1) / pclk;
}

uint16_t Spi::take_miso(unsigned bytes) {
  uint16_t v = 0;
  for (unsigned i = 0; i < bytes; ++i) {
    uint8_t b = 0xFF;
    if (miso_head_ < miso_q_.size()) b = miso_q_[miso_head_++];
    v |= static_cast<uint16_t>(b) << (8 * i);
  }
  if (miso_head_ == miso_q_.size()) {
    miso_q_.clear();
    miso_head_ = 0;
  }
  return v;
}

void Spi::start_next() {
  if (shifting_ || tx_.n == 0 || !(regs_.get(CR1) & SPE)) return;
  const uint64_t dt = frame_ns();  // may throw: nothing changed yet
  cur_ = tx_.pop();
  if (mosi_.empty()) burst_start_ = clock_.now_ns();
  shifting_ = true;
  ev_ = clock_.schedule_at(clock_.now_ns() + dt, [this] { complete(); });
}

void Spi::complete() {
  shifting_ = false;
  const unsigned ds = data_bits();
  const uint16_t mask = static_cast<uint16_t>((1u << ds) - 1u);
  Frame in{static_cast<uint16_t>(take_miso(cur_.bytes) & mask), cur_.bytes};
  if (rx_.bytes + in.bytes <= kFifoBytes) rx_.push(in);
  else ovr_ = true;
  append_hex(mosi_, cur_.v, cur_.bytes);
  append_hex(miso_, in.v, in.bytes);
  start_next();
  if (!shifting_) {
    emit_event(clock_.now_ns(), {{"kind", "spi"}, {"bus", name_}, {"t_start", burst_start_},
                                 {"mosi", mosi_}, {"miso", miso_}});
    mosi_.clear();
    miso_.clear();
  }
  sync();
}

void Spi::sync() {
  const uint32_t cr2 = regs_.get(CR2);
  uint32_t sr = 0;
  const unsigned rx_th = (cr2 & FRXTH) ? 1 : 2;
  if (rx_.bytes >= rx_th) sr |= RXNE;
  if (tx_.bytes <= kFifoBytes / 2) sr |= TXE;
  if (ovr_) sr |= OVR;
  if (shifting_) sr |= BSY;
  sr |= level_code(rx_.bytes) << FRLVL_POS;
  sr |= level_code(tx_.bytes) << FTLVL_POS;
  regs_.set(SR, sr);
  const bool lv = ((cr2 & TXEIE) && (sr & TXE)) || ((cr2 & RXNEIE) && (sr & RXNE)) ||
                  ((cr2 & ERRIE) && (sr & OVR));
  if (irq_ >= 0 && lv != level_) {
    level_ = lv;
    core_.set_irq_level(irq_, lv);
  }
}

uint32_t Spi::read_dr(unsigned width) {
  uint32_t v = 0;
  if (rx_.n > 0) {
    const Frame a = rx_.pop();
    v = a.v;
    if (width >= 2 && a.bytes == 1 && rx_.n > 0 && rx_.f[0].bytes == 1) v |= uint32_t{rx_.pop().v} << 8;
  }
  dr_read_ = true;
  sync();
  return v;
}

void Spi::write_dr(unsigned width, uint32_t v) {
  const unsigned ds = data_bits();
  const uint16_t mask = static_cast<uint16_t>((1u << ds) - 1u);
  const uint8_t fb = ds > 8 ? 2 : 1;
  Frame f[2];
  unsigned n = 0;
  if (fb == 1 && width >= 2) {
    f[n++] = {static_cast<uint16_t>(v & 0xFFu & mask), 1};
    f[n++] = {static_cast<uint16_t>((v >> 8) & 0xFFu & mask), 1};
  } else {
    f[n++] = {static_cast<uint16_t>(v & (width == 1 ? 0xFFu : 0xFFFFu) & mask), fb};
  }
  if (tx_.bytes + n * fb > kFifoBytes) {
    char b[96];
    std::snprintf(b, sizeof b, "write%u to DR with the TX FIFO full (%u of 4 bytes used)", width * 8,
                  tx_.bytes);
    not_modelled(b);
  }
  for (unsigned i = 0; i < n; ++i) tx_.push(f[i]);
  start_next();
  sync();
}

uint32_t Spi::read(uint32_t off, unsigned width) {
  if ((off & ~3u) == DR) {
    if (off != DR) throw UnmodelledRegister(off, width, false);
    return read_dr(width);
  }
  const uint32_t v = regs_.read(off, width);
  if ((off & ~3u) == SR) {
    if (dr_read_ && ovr_) {  // OVR clears on a DR read followed by an SR read
      ovr_ = false;
      sync();
    }
    dr_read_ = false;
  }
  return v;
}

void Spi::write(uint32_t off, unsigned width, uint32_t v) {
  const uint32_t aligned = off & ~3u;
  if (aligned == DR) {
    if (off != DR) throw UnmodelledRegister(off, width, true);
    write_dr(width, v);
    return;
  }
  if (aligned == CR1 || aligned == CR2) {
    uint32_t cr1 = regs_.get(CR1), cr2 = regs_.get(CR2);
    const uint32_t full = lane_merge(aligned == CR1 ? cr1 : cr2, off, width, v);
    if (aligned == CR1) cr1 = full & 0xFFFFu;
    else {
      cr2 = full & 0x7FFFu;
      if (((cr2 & DS) >> DS_POS) < 3) cr2 = (cr2 & ~DS) | (7u << DS_POS);  // reserved -> 8-bit
    }
    check_config(cr1, cr2, aligned, full);
    regs_.write(off, width, v);
    regs_.set(aligned, aligned == CR1 ? cr1 : cr2);
    start_next();
    sync();
    return;
  }
  regs_.write(off, width, v);  // SR (CRCERR only), CRCPR; RXCRCR/TXCRCR are read-only
  if (aligned == SR) sync();
}

nlohmann::json Spi::call(std::string_view op, const nlohmann::json& args) {
  if (op == "queue_miso") {
    if (!args.contains("data") || !args["data"].is_string())
      throw std::invalid_argument(name_ + ": queue_miso needs {\"data\": \"<hex>\"}");
    const std::string s = args["data"].get<std::string>();
    if (s.size() % 2) throw std::invalid_argument(name_ + ": queue_miso data has an odd number of hex digits");
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < s.size(); i += 2) {
      const int hi = hexval(s[i]), lo = hexval(s[i + 1]);
      if (hi < 0 || lo < 0) throw std::invalid_argument(name_ + ": queue_miso data is not hex: " + s);
      bytes.push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    miso_q_.insert(miso_q_.end(), bytes.begin(), bytes.end());
    return {{"queued", miso_q_.size() - miso_head_}};
  }
  if (op == "state") {
    const uint32_t cr1 = regs_.get(CR1);
    const uint64_t pclk = clocks_ ? pclk_hz() : 0;
    const uint64_t div = 2ull << ((cr1 & BR) >> BR_POS);
    return {{"enabled", (cr1 & SPE) != 0},
            {"master", (cr1 & MSTR) != 0},
            {"data_bits", data_bits()},
            {"cpol", (cr1 & CPOL) ? 1 : 0},
            {"cpha", (cr1 & CPHA) ? 1 : 0},
            {"lsbfirst", (cr1 & LSBFIRST) != 0},
            {"baud_hz", static_cast<double>(pclk) / static_cast<double>(div)},
            {"frame_ns", pclk ? frame_ns() : 0},
            {"busy", shifting_},
            {"tx_level", tx_.bytes},
            {"rx_level", rx_.bytes},
            {"ovr", ovr_},
            {"miso_queued", miso_q_.size() - miso_head_}};
  }
  throw UnknownOp(op);
}

}  // namespace stest::stm32l4
