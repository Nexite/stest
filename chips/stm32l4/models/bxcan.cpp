#include "stm32l4/models/bxcan.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include "stest/node.hpp"

namespace stest::stm32l4 {
namespace {
enum : uint32_t {
  MCR = 0x00, MSR = 0x04, TSR = 0x08, RF0R = 0x0C, RF1R = 0x10, IER = 0x14, ESR = 0x18, BTR = 0x1C,
  TI0R = 0x180, RI0R = 0x1B0, FIFO_END = 0x1D0,
  FMR = 0x200, FM1R = 0x204, FS1R = 0x20C, FFA1R = 0x214, FA1R = 0x21C, F0R1 = 0x240, F_END = 0x320,
};
constexpr unsigned kBanks = 28;
constexpr uint32_t kBankMask = (1u << kBanks) - 1u;
// MCR (CAN_MCR_*)
constexpr uint32_t INRQ = 1u << 0, SLEEP = 1u << 1, TXFP = 1u << 2, RFLM = 1u << 3, NART = 1u << 4,
                   AWUM = 1u << 5, ABOM = 1u << 6, TTCM = 1u << 7, RESET = 1u << 15, DBF = 1u << 16;
constexpr uint32_t MCR_RW = INRQ | SLEEP | TXFP | RFLM | NART | AWUM | ABOM | TTCM | DBF;
constexpr uint32_t MCR_RESET = DBF | SLEEP;
// MSR (CAN_MSR_*)
constexpr uint32_t INAK = 1u << 0, SLAK = 1u << 1, ERRI = 1u << 2, WKUI = 1u << 3, SLAKI = 1u << 4,
                   SAMP = 1u << 10, RXBIT = 1u << 11;
constexpr uint32_t MSR_RESET = RXBIT | SAMP | SLAK;
// TSR (CAN_TSR_*): mailbox m's bits at << 8m
constexpr uint32_t RQCP = 1u << 0, TXOK = 1u << 1, ALST = 1u << 2, TERR = 1u << 3, ABRQ = 1u << 7;
constexpr uint32_t CODE_POS = 24, TME_POS = 26, LOW_POS = 29;
// RFxR
constexpr uint32_t FULL = 1u << 3, FOVR = 1u << 4, RFOM = 1u << 5;
// IER (CAN_IER_*)
constexpr uint32_t TMEIE = 1u << 0, FMPIE0 = 1u << 1, FFIE0 = 1u << 2, FOVIE0 = 1u << 3,
                   EWGIE = 1u << 8, EPVIE = 1u << 9, BOFIE = 1u << 10, LECIE = 1u << 11,
                   ERRIE = 1u << 15, WKUIE = 1u << 16, SLKIE = 1u << 17;
constexpr uint32_t IER_RW = 0x7Fu | EWGIE | EPVIE | BOFIE | LECIE | ERRIE | WKUIE | SLKIE;
// ESR (CAN_ESR_*)
constexpr uint32_t EWGF = 1u << 0, EPVF = 1u << 1, BOFF = 1u << 2, LEC_POS = 4, LEC = 7u << 4;
// BTR (CAN_BTR_*)
constexpr uint32_t BTR_RESET = 0x01230000u, BRP = 0x3FFu, TS1_POS = 16, TS2_POS = 20,
                   LBKM = 1u << 30, SILM = 1u << 31;
constexpr uint32_t BTR_RW = BRP | (0xFu << 16) | (7u << 20) | (3u << 24) | LBKM | SILM;
// TIxR / RIxR
constexpr uint32_t TXRQ = 1u << 0, RTR = 1u << 1, IDE = 1u << 2;
// FMR
constexpr uint32_t FINIT = 1u << 0, CAN2SB_POS = 8, CAN2SB = 0x3Fu << 8, FMR_RESET = 0x2A1C0E01u;

enum Irq { kTx, kRx0, kRx1, kSce };

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
// 32-bit filter / mailbox identifier word: STID[31:21] | EXID[31:3], IDE bit 2, RTR bit 1.
uint32_t id32(uint32_t id, bool ext, bool rtr) {
  return (ext ? (id << 3) | IDE : id << 21) | (rtr ? RTR : 0u);
}
// 16-bit filter word: STID[10:0] at [15:5], RTR bit 4, IDE bit 3, EXID[17:15] at [2:0].
uint32_t id16(uint32_t id, bool ext, bool rtr) {
  const uint32_t stid = ext ? id >> 18 : id;
  return (stid << 5) | (rtr ? 0x10u : 0u) | (ext ? 0x08u | ((id >> 15) & 7u) : 0u);
}
bool is_bool(const nlohmann::json& a, const char* k) { return a.contains(k) && a[k].is_boolean(); }
const char* const kLecNames[] = {"", "stuff", "form", "ack", "bit_recessive", "bit_dominant", "crc"};
// a non-negative integer no larger than max (JSON ints may be stored signed or unsigned)
bool is_uint(const nlohmann::json& a, const char* k, uint64_t max) {
  if (!a.contains(k) || !a[k].is_number_integer()) return false;
  if (a[k].is_number_unsigned()) return a[k].get<uint64_t>() <= max;
  const int64_t v = a[k].get<int64_t>();
  return v >= 0 && static_cast<uint64_t>(v) <= max;
}
}  // namespace

Bxcan::Bxcan(std::string name, const nlohmann::json& params, ModelContext& ctx)
    : name_(std::move(name)), clock_(ctx.clock()), ctx_(ctx) {
  const std::string role = params.value("role", "");
  if (role != "master" && role != "slave")
    throw std::invalid_argument(name_ + ": bxcan params.role must be \"master\" or \"slave\"");
  master_ = role == "master";
  master_name_ = params.value("master", std::string("CAN1"));
  if (params.contains("irq")) {
    const auto& irq = params["irq"];
    if (!irq.is_object()) throw std::invalid_argument(name_ + ": bxcan params.irq must be an object");
    const char* keys[4] = {"tx", "rx0", "rx1", "sce"};
    for (int i = 0; i < 4; ++i) irq_[i] = irq.value(keys[i], -1);
  }
  regs_.declare(MCR, MCR_RESET, 0);  // model-handled (write_mcr)
  regs_.declare(MSR, MSR_RESET, 0, ERRI | WKUI | SLAKI);
  regs_.declare(TSR, 7u << TME_POS, 0);    // model-handled (write_tsr)
  regs_.declare(RF0R, 0, 0, FULL | FOVR);  // + RFOM, model-handled
  regs_.declare(RF1R, 0, 0, FULL | FOVR);
  regs_.declare(IER, 0, IER_RW);
  regs_.declare(ESR, 0, LEC);
  regs_.declare(BTR, BTR_RESET, BTR_RW);
  for (unsigned m = 0; m < 3; ++m) {
    regs_.declare(TI0R + 0x10 * m, 0, 0xFFFFFFFFu);
    regs_.declare(TI0R + 0x10 * m + 4, 0, 0x0000010Fu);  // DLC, TGT (TIME reads 0)
    regs_.declare(TI0R + 0x10 * m + 8, 0, 0xFFFFFFFFu);
    regs_.declare(TI0R + 0x10 * m + 12, 0, 0xFFFFFFFFu);
  }
  for (uint32_t off = RI0R; off < FIFO_END; off += 4) regs_.declare(off, 0, 0);  // read-only
  if (master_) {
    regs_.declare(FMR, FMR_RESET, FINIT | CAN2SB);
    for (uint32_t off : {FM1R, FS1R, FFA1R, FA1R}) regs_.declare(off, 0, kBankMask);
    for (uint32_t off = F0R1; off < F_END; off += 4) regs_.declare(off, 0, 0xFFFFFFFFu);
  }
}

void Bxcan::connect(ModelContext& ctx) {
  clocks_ = &ctx.clocks();
  filters_ = master_ ? this : &ctx.get<Bxcan>(master_name_);
  if (!filters_->master_) throw std::logic_error(name_ + ": bxcan master '" + master_name_ + "' has role slave");
  ctx.clock_tree_changed().subscribe([this](const ClockTree&) {
    if (mode_ != Mode::Normal) return;
    const uint64_t b = bitrate();
    if (b == reported_bitrate_) return;
    emit_mode();
  });
}

void Bxcan::reset() {
  if (syncing_) clock_.cancel(sync_ev_);
  syncing_ = false;
  mode_ = Mode::Sleep;
  mb_ = {};
  fifo_ = {};
  seq_ = order_ = 0;
  slaki_ = erri_ = false;
  reported_bitrate_ = 0;
  tec_ = rec_ = 0;
  epoch_ = 0;
  regs_.reset();
  sync();
}

void Bxcan::not_modelled(const std::string& what) const { throw NotModelled(name_, what); }

uint64_t Bxcan::bitrate() const {
  if (!clocks_) return 0;
  const uint32_t btr = regs_.get(BTR);
  const uint64_t tq = 1 + ((btr >> TS1_POS) & 0xF) + 1 + ((btr >> TS2_POS) & 7) + 1;
  return clocks_->pclk1_hz() / (((btr & BRP) + 1) * tq);
}

uint64_t Bxcan::bit_ns() const {
  if (!clocks_) throw std::logic_error(name_ + ": not connected to a clock tree");
  const uint64_t pclk = clocks_->pclk1_hz();
  if (pclk == 0) throw std::runtime_error(name_ + ": PCLK1 is 0 Hz");
  const uint32_t btr = regs_.get(BTR);
  const uint64_t clocks = ((btr & BRP) + 1) * (3 + ((btr >> TS1_POS) & 0xF) + ((btr >> TS2_POS) & 7));
  return (clocks * 1000000000ull + pclk - 1) / pclk;
}

// ---- modes ----

void Bxcan::emit_mode(bool master_reset) {
  static const char* names[] = {"sleep", "init", "normal"};
  nlohmann::json b = nullptr;
  if (mode_ == Mode::Normal) {
    reported_bitrate_ = bitrate();
    b = reported_bitrate_;
  }
  reported_abom_ = (regs_.get(MCR) & ABOM) != 0;
  nlohmann::json ev = {{"kind", "can_mode"}, {"periph", name_}, {"mode", names[static_cast<int>(mode_)]},
                       {"bitrate", b}, {"abom", reported_abom_}};
  if (master_reset) ev["reset"] = true;
  emit_event(clock_.now_ns(), ev);
}

void Bxcan::apply_mode() {
  const uint32_t mcr = regs_.get(MCR);
  const Mode want = (mcr & INRQ) ? Mode::Init : (mcr & SLEEP) ? Mode::Sleep : Mode::Normal;
  if (want != Mode::Normal) {
    if (syncing_) {
      clock_.cancel(sync_ev_);
      syncing_ = false;
    }
    if (mode_ != want) enter(want);
    return;
  }
  if (mode_ == Mode::Normal || syncing_) return;
  syncing_ = true;  // 11 recessive bits (bus assumed idle)
  sync_ev_ = clock_.schedule_at(clock_.now_ns() + 11 * bit_ns(), [this] {
    syncing_ = false;
    enter(Mode::Normal);
  });
}

void Bxcan::enter(Mode m) {
  const bool was_normal = mode_ == Mode::Normal;
  mode_ = m;
  if (m == Mode::Sleep && (regs_.get(IER) & SLKIE)) slaki_ = true;
  emit_mode();
  if (was_normal) {  // withdraw requests from the bus (hardware keeps them pending)
    for (unsigned i = 0; i < 3; ++i) {
      Mailbox& b = mb_[i];
      if (b.st != Tx::Requested || b.abort || b.withdraw) continue;
      b.withdraw = true;
      emit_event(clock_.now_ns(), {{"kind", "can_tx_abort"}, {"periph", name_}, {"mailbox", i}, {"seq", b.seq}});
    }
  }
  if (m == Mode::Normal) {  // held requests, in TXRQ order
    for (;;) {
      int best = -1;
      for (unsigned i = 0; i < 3; ++i)
        if (mb_[i].st == Tx::Held && (best < 0 || mb_[i].order < mb_[best].order)) best = static_cast<int>(i);
      if (best < 0) break;
      request(static_cast<unsigned>(best));
    }
  }
  sync();
}

void Bxcan::master_reset() {
  for (unsigned i = 0; i < 3; ++i) {
    const Mailbox& b = mb_[i];
    if (b.st == Tx::Requested && !b.abort && !b.withdraw)
      emit_event(clock_.now_ns(), {{"kind", "can_tx_abort"}, {"periph", name_}, {"mailbox", i}, {"seq", b.seq}});
  }
  if (syncing_) clock_.cancel(sync_ev_);
  syncing_ = false;
  mb_ = {};
  fifo_ = {};
  slaki_ = erri_ = false;
  tec_ = rec_ = 0;  // ESR resets below; deliveries carrying older counters are dropped (epoch)
  ++epoch_;
  // every register except the filters (they are not part of the CAN cell's master reset)
  const uint32_t rst[][2] = {{MCR, MCR_RESET}, {MSR, MSR_RESET}, {IER, 0}, {ESR, 0}, {BTR, BTR_RESET}};
  for (const auto& r : rst) regs_.set(r[0], r[1]);
  for (uint32_t off = TI0R; off < FIFO_END; off += 4) regs_.set(off, 0);
  mode_ = Mode::Sleep;
  emit_mode(true);  // always: the bus engine zeroes its counters for this controller
  sync();
}

void Bxcan::write_mcr(uint32_t v) {
  if (v & RESET) {
    master_reset();
    return;
  }
  if (v & TTCM) not_modelled("time-triggered communication mode (MCR.TTCM)");
  if (v & AWUM) not_modelled("automatic wake-up on bus activity (MCR.AWUM)");
  regs_.set(MCR, v & MCR_RW);
  apply_mode();
  // ABOM changed while in normal mode: re-report it (otherwise the next can_mode carries it)
  if (mode_ == Mode::Normal && ((v & ABOM) != 0) != reported_abom_) emit_mode();
  sync();
}

// ---- TX ----

uint64_t Bxcan::arb_key(unsigned m) const {
  // CAN arbitration order (lower wins): base ID, SRR/RTR, IDE, extension ID, RTR.
  const uint32_t tir = regs_.get(TI0R + 0x10 * m);
  const bool ext = tir & IDE, rtr = tir & RTR;
  if (!ext) return (uint64_t{tir >> 21} << 21) | (uint64_t{rtr} << 20);
  const uint32_t id = tir >> 3;
  return (uint64_t{id >> 18} << 21) | (1ull << 20) | (1ull << 19) | (uint64_t{id & 0x3FFFFu} << 1) | rtr;
}

void Bxcan::request(unsigned m) {
  Mailbox& b = mb_[m];
  const uint32_t base = TI0R + 0x10 * m;
  const uint32_t tir = regs_.get(base), dlc = regs_.get(base + 4) & 0xF;
  const bool ext = tir & IDE, rtr = tir & RTR;
  const uint32_t id = ext ? tir >> 3 : tir >> 21;
  std::string data;
  if (!rtr) {
    static const char* d = "0123456789abcdef";
    const uint64_t bytes = regs_.get(base + 8) | (uint64_t{regs_.get(base + 12)} << 32);
    for (unsigned i = 0; i < dlc; ++i) {
      const unsigned x = (bytes >> (8 * i)) & 0xFF;
      data.push_back(d[x >> 4]);
      data.push_back(d[x & 0xF]);
    }
  }
  const uint32_t mcr = regs_.get(MCR);
  b.st = Tx::Requested;
  b.seq = ++seq_;
  b.withdraw = false;
  emit_event(clock_.now_ns(), {{"kind", "can_tx"}, {"periph", name_}, {"mailbox", m}, {"id", id}, {"ext", ext},
                               {"rtr", rtr}, {"dlc", dlc}, {"data", data}, {"txfp", (mcr & TXFP) ? 1 : 0},
                               {"nart", (mcr & NART) != 0}, {"seq", b.seq}});
}

void Bxcan::complete(unsigned m) {
  Mailbox& b = mb_[m];
  b.st = Tx::Empty;
  b.abort = b.withdraw = false;
  b.rqcp = true;
  const uint32_t base = TI0R + 0x10 * m;
  regs_.set(base, regs_.get(base) & ~TXRQ);
}

void Bxcan::write_mailbox(unsigned m, uint32_t off, unsigned width, uint32_t v) {
  Mailbox& b = mb_[m];
  const uint32_t base = TI0R + 0x10 * m;
  if (b.st != Tx::Empty) {
    static const char* regn[] = {"TIR", "TDTR", "TDLR", "TDHR"};
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "write to TX mailbox %u %s while it is pending (TSR.TME%u = 0; write-protected, RM0351 55.9.3)",
                  m, regn[((off & ~3u) - base) / 4], m);
    not_modelled(buf);
  }
  if ((off & ~3u) == base) {
    const uint32_t full = lane_merge(regs_.get(base), off, width, v);
    if ((full & TXRQ) && (regs_.get(base + 4) & 0xF) > 8)
      not_modelled("TX data length code > 8 (TDTR.DLC 9-15 at TXRQ)");
  }
  regs_.write(off, width, v);
  if (!(regs_.get(base) & TXRQ)) return;
  b.order = ++order_;
  b.abort = b.withdraw = false;
  b.rqcp = b.txok = b.alst = b.terr = false;  // RQCPx "cleared by hardware on TXRQx set" (TSR)
  if (mode_ == Mode::Normal) request(m);
  else b.st = Tx::Held;
  sync();
}

void Bxcan::write_tsr(uint32_t w) {
  for (unsigned m = 0; m < 3; ++m) {
    Mailbox& b = mb_[m];
    const uint32_t bits = w >> (8 * m);
    if (bits & RQCP) b.rqcp = b.txok = b.alst = b.terr = false;
    if (!(bits & ABRQ) || b.st == Tx::Empty || b.abort) continue;
    if (b.st == Tx::Held) {
      b.txok = false;
      complete(m);
      continue;
    }
    b.abort = true;
    if (!b.withdraw)
      emit_event(clock_.now_ns(), {{"kind", "can_tx_abort"}, {"periph", name_}, {"mailbox", m}, {"seq", b.seq}});
  }
  sync();
}

// outcome: 0 ok, 1 arb_lost, 2 error (lec), 3 aborted
void Bxcan::on_tx_result(unsigned m, uint64_t seq, int outcome, uint32_t lec) {
  Mailbox& b = mb_[m];
  if (b.st != Tx::Requested || b.seq != seq) return;  // stale
  switch (outcome) {
    case 0:
      b.txok = true;
      b.alst = b.terr = false;
      set_lec(0);
      complete(m);
      break;
    case 1:
    case 2:
      b.txok = false;
      b.alst = outcome == 1;
      b.terr = outcome == 2;
      if (outcome == 2) set_lec(lec);
      if (regs_.get(MCR) & NART) complete(m);
      break;
    default:
      if (b.abort) {
        b.txok = false;
        complete(m);
      } else if (b.withdraw) {
        b.withdraw = false;
        if (mode_ == Mode::Normal) request(m);
        else b.st = Tx::Held;
      } else {
        throw std::logic_error(name_ + ": tx_result \"aborted\" for mailbox " + std::to_string(m) +
                               " seq " + std::to_string(seq) + ", which was not aborted");
      }
  }
  sync();
}

void Bxcan::set_counters(uint32_t tec, uint32_t rec) {
  tec_ = tec;
  rec_ = rec;
  const uint32_t esr = regs_.get(ESR);
  const uint32_t flags = (tec >= 96 || rec >= 96 ? EWGF : 0u) | (tec > 127 || rec > 127 ? EPVF : 0u) |
                         (tec > 255 ? BOFF : 0u);
  const uint32_t rising = flags & ~esr;
  regs_.set(ESR, (esr & LEC) | flags | ((tec & 0xFFu) << 16) | (std::min<uint32_t>(rec, 255) << 24));
  const uint32_t ier = regs_.get(IER);
  if (((rising & EWGF) && (ier & EWGIE)) || ((rising & EPVF) && (ier & EPVIE)) || ((rising & BOFF) && (ier & BOFIE)))
    erri_ = true;
}

void Bxcan::set_lec(uint32_t lec) {
  regs_.set(ESR, (regs_.get(ESR) & ~LEC) | (lec << LEC_POS));
  if (lec && (regs_.get(IER) & LECIE)) erri_ = true;
}

// ---- RX ----

bool Bxcan::match(const Frame& f, unsigned& fifo, uint8_t& fmi) const {
  const RegisterFile& fr = filters_->regs_;
  if (fr.get(FMR) & FINIT) return false;
  unsigned sb = (fr.get(FMR) & CAN2SB) >> CAN2SB_POS;
  const unsigned lo = master_ ? 0 : sb, hi = master_ ? sb : kBanks;
  const uint32_t fm = fr.get(FM1R), fs = fr.get(FS1R), ffa = fr.get(FFA1R), fa = fr.get(FA1R);
  const uint32_t w32 = id32(f.id, f.ext, f.rtr), w16 = id16(f.id, f.ext, f.rtr);
  unsigned num[2] = {0, 0};
  int best_rank = 99;
  for (unsigned b = lo; b < hi; ++b) {
    const bool s32 = fs >> b & 1, list = fm >> b & 1;
    const unsigned q = ffa >> b & 1;
    const unsigned n = s32 ? (list ? 2 : 1) : (list ? 4 : 2);
    const int rank = (s32 ? 0 : 2) + (list ? 0 : 1);
    if ((fa >> b & 1) && rank < best_rank) {
      const uint32_t r1 = fr.get(F0R1 + 8 * b), r2 = fr.get(F0R1 + 8 * b + 4);
      int k = -1;
      if (s32 && list) k = ((w32 ^ r1) & ~1u) == 0 ? 0 : ((w32 ^ r2) & ~1u) == 0 ? 1 : -1;
      else if (s32) k = ((w32 ^ r1) & r2) == 0 ? 0 : -1;
      else if (list) {
        const uint32_t ids[4] = {r1 & 0xFFFF, r1 >> 16, r2 & 0xFFFF, r2 >> 16};
        for (int i = 0; i < 4 && k < 0; ++i)
          if (ids[i] == w16) k = i;
      } else {
        if (((w16 ^ r1) & (r1 >> 16) & 0xFFFF) == 0) k = 0;
        else if (((w16 ^ r2) & (r2 >> 16) & 0xFFFF) == 0) k = 1;
      }
      if (k >= 0) {
        best_rank = rank;
        fifo = q;
        fmi = static_cast<uint8_t>(num[q] + static_cast<unsigned>(k));
      }
    }
    num[q] += n;
  }
  return best_rank != 99;
}

void Bxcan::show_fifo(unsigned q) {
  const Fifo& ff = fifo_[q];
  if (ff.n == 0) return;  // the output mailbox keeps its last contents
  const Frame& f = ff.msg[0];
  const uint32_t base = RI0R + 0x10 * q;
  uint32_t lo = 0, hi = 0;
  for (unsigned i = 0; i < 4; ++i) lo |= uint32_t{f.data[i]} << (8 * i);
  for (unsigned i = 0; i < 4; ++i) hi |= uint32_t{f.data[4 + i]} << (8 * i);
  regs_.set(base, id32(f.id, f.ext, f.rtr));
  regs_.set(base + 4, f.dlc | (uint32_t{ff.fmi[0]} << 8));
  regs_.set(base + 8, lo);
  regs_.set(base + 12, hi);
}

void Bxcan::on_rx(const Frame& f) {
  if (mode_ != Mode::Normal) return;
  set_lec(0);
  unsigned q = 0;
  uint8_t fmi = 0;
  if (match(f, q, fmi)) {
    Fifo& ff = fifo_[q];
    if (ff.n < 3) {
      ff.msg[ff.n] = f;
      ff.fmi[ff.n] = fmi;
      ++ff.n;
      if (ff.n == 3) ff.full = true;
    } else {
      ff.ovr = true;
      if (!(regs_.get(MCR) & RFLM)) {
        ff.msg[2] = f;
        ff.fmi[2] = fmi;
      }
    }
    show_fifo(q);
  }
  sync();
}

// ---- filters ----

bool Bxcan::filter_reg(uint32_t aligned) const { return aligned >= FMR && aligned < F_END; }

void Bxcan::write_filter(uint32_t aligned, uint32_t off, unsigned width, uint32_t v) {
  const bool finit = regs_.get(FMR) & FINIT;
  if (aligned == FMR) {
    const uint32_t full = lane_merge(regs_.get(FMR), off, width, v);
    if (((full & CAN2SB) >> CAN2SB_POS) > kBanks) not_modelled("FMR.CAN2SB > 28 (there are 28 filter banks)");
  } else if (aligned == FM1R || aligned == FS1R || aligned == FFA1R) {
    if (!finit) {
      static const char* n[] = {"FM1R", "", "FS1R", "", "FFA1R"};
      not_modelled(std::string(n[(aligned - FM1R) / 4]) +
                   " write with FMR.FINIT = 0 (write-protected outside filter initialization mode)");
    }
  } else if (aligned >= F0R1) {
    const unsigned bank = (aligned - F0R1) / 8;
    if (!finit && (regs_.get(FA1R) >> bank & 1)) {
      char buf[128];
      std::snprintf(buf, sizeof buf,
                    "F%uR%u write while bank %u is active and FMR.FINIT = 0 (write-protected)", bank,
                    ((aligned - F0R1) / 4) % 2 + 1, bank);
      not_modelled(buf);
    }
  }
  regs_.write(off, width, v);
}

// ---- status, IRQs ----

void Bxcan::sync() {
  // TSR
  uint32_t tsr = 0;
  int pending = 0, free_mb = -1, low = -1;
  const bool by_order = regs_.get(MCR) & TXFP;
  for (unsigned m = 0; m < 3; ++m) {
    const Mailbox& b = mb_[m];
    tsr |= ((b.rqcp ? RQCP : 0u) | (b.txok ? TXOK : 0u) | (b.alst ? ALST : 0u) | (b.terr ? TERR : 0u) |
            (b.abort && b.st != Tx::Empty ? ABRQ : 0u))
           << (8 * m);
    if (b.st == Tx::Empty) {
      tsr |= 1u << (TME_POS + m);
      if (free_mb < 0) free_mb = static_cast<int>(m);
      continue;
    }
    ++pending;
    if (low < 0) {
      low = static_cast<int>(m);
      continue;
    }
    const bool lower = by_order ? b.order > mb_[low].order : arb_key(m) >= arb_key(static_cast<unsigned>(low));
    if (lower) low = static_cast<int>(m);
  }
  tsr |= static_cast<uint32_t>(free_mb >= 0 ? free_mb : low) << CODE_POS;
  if (pending > 1) tsr |= 1u << (LOW_POS + static_cast<unsigned>(low));
  regs_.set(TSR, tsr);
  for (unsigned q = 0; q < 2; ++q)
    regs_.set(q ? RF1R : RF0R, fifo_[q].n | (fifo_[q].full ? FULL : 0u) | (fifo_[q].ovr ? FOVR : 0u));
  uint32_t msr = regs_.get(MSR) & ~(INAK | SLAK | ERRI | SLAKI);
  if (mode_ == Mode::Init) msr |= INAK;  // until the 11-bit synchronisation completes
  if (mode_ == Mode::Sleep) msr |= SLAK;
  if (erri_) msr |= ERRI;
  if (slaki_) msr |= SLAKI;
  regs_.set(MSR, msr);
  // IRQ levels
  const uint32_t ier = regs_.get(IER);
  bool lv[4];
  lv[kTx] = (ier & TMEIE) && (tsr & (RQCP | RQCP << 8 | RQCP << 16));
  for (unsigned q = 0; q < 2; ++q) {
    const uint32_t e = ier >> (3 * q);
    lv[kRx0 + q] = ((e & FMPIE0) && fifo_[q].n) || ((e & FFIE0) && fifo_[q].full) || ((e & FOVIE0) && fifo_[q].ovr);
  }
  lv[kSce] = ((ier & ERRIE) && erri_) || ((ier & SLKIE) && slaki_);
  for (int i = 0; i < 4; ++i) {
    if (irq_[i] < 0 || lv[i] == level_[i]) continue;
    level_[i] = lv[i];
    ctx_.set_irq_source(irq_[i], &irq_tag_[i], lv[i]);
  }
}

// ---- MMIO ----

uint32_t Bxcan::read(uint32_t off, unsigned width) { return regs_.read(off, width); }

void Bxcan::write(uint32_t off, unsigned width, uint32_t v) {
  const uint32_t aligned = off & ~3u;
  const uint32_t lane = (v << ((off & 3u) * 8)) & lane_mask(off, width);  // only the written bits
  switch (aligned) {
    case MCR:
      write_mcr(lane_merge(regs_.get(MCR), off, width, v));
      return;
    case MSR:
      regs_.write(off, width, v);
      if (lane & ERRI) erri_ = false;
      if (lane & SLAKI) slaki_ = false;
      sync();
      return;
    case TSR:
      write_tsr(lane);
      return;
    case RF0R:
    case RF1R: {
      Fifo& ff = fifo_[aligned == RF1R];
      if (lane & FULL) ff.full = false;
      if (lane & FOVR) ff.ovr = false;
      if ((lane & RFOM) && ff.n) {
        for (unsigned i = 1; i < ff.n; ++i) {
          ff.msg[i - 1] = ff.msg[i];
          ff.fmi[i - 1] = ff.fmi[i];
        }
        --ff.n;
        show_fifo(aligned == RF1R);
      }
      sync();
      return;
    }
    case IER: {
      const uint32_t full = lane_merge(regs_.get(IER), off, width, v);
      if (full & WKUIE) not_modelled("wake-up interrupt on bus activity (IER.WKUIE)");
      regs_.write(off, width, v);
      sync();
      return;
    }
    case BTR: {
      if (mode_ != Mode::Init)
        not_modelled("BTR write outside initialization mode (MSR.INAK = 0; BTR is write-protected then)");
      const uint32_t full = lane_merge(regs_.get(BTR), off, width, v);
      if (full & LBKM) not_modelled("loop back mode (BTR.LBKM)");
      if (full & SILM) not_modelled("silent mode (BTR.SILM)");
      regs_.write(off, width, v);
      return;
    }
    default:
      break;
  }
  if (aligned >= TI0R && aligned < RI0R) {
    write_mailbox((aligned - TI0R) / 0x10, off, width, v);
    return;
  }
  if (master_ && filter_reg(aligned)) {
    write_filter(aligned, off, width, v);
    return;
  }
  regs_.write(off, width, v);  // ESR (LEC), read-only RX mailboxes (abort), unmodelled (abort)
  if (aligned == ESR) sync();
}

// ---- Python ----

nlohmann::json Bxcan::call(std::string_view op, const nlohmann::json&) {
  if (op == "state") {
    static const char* modes[] = {"sleep", "init", "normal"};
    static const char* sts[] = {"empty", "held", "pending"};
    nlohmann::json tx = nlohmann::json::array();
    for (const Mailbox& b : mb_)
      tx.push_back({{"state", sts[static_cast<int>(b.st)]},
                    {"seq", b.st == Tx::Requested ? nlohmann::json(b.seq) : nlohmann::json(nullptr)},
                    {"abort", b.abort && b.st != Tx::Empty}});
    return {{"mode", modes[static_cast<int>(mode_)]},
            {"bitrate", mode_ == Mode::Normal ? nlohmann::json(bitrate()) : nlohmann::json(nullptr)},
            {"tec", tec_},
            {"rec", rec_},
            {"error_state", tec_ > 255 ? "bus_off" : (tec_ > 127 || rec_ > 127) ? "passive" : "active"},
            {"esr", regs_.get(ESR)},
            {"msr", regs_.get(MSR)},
            {"tsr", regs_.get(TSR)},
            {"tx", tx},
            {"fifo", {fifo_[0].n, fifo_[1].n}}};
  }
  if (op == "can_info") return {{"type", "bxcan"}, {"role", master_ ? "master" : "slave"}};
  throw UnknownOp(op);
}

void Bxcan::deliver(std::string_view op, const nlohmann::json& a, uint64_t t_ns) {
  const auto bad = [&](const std::string& why) { throw std::invalid_argument(name_ + " " + std::string(op) + ": " + why); };
  if (!a.is_object()) bad("args must be an object");
  // Fault-confinement counters from the bus engine (absolute values): "tec", "rec" and
  // "epoch" together, optional on tx_result / rx, required on error / counters.
  struct Fc {
    bool has = false;
    uint32_t tec = 0, rec = 0;
    uint64_t epoch = 0;
  } fc;
  const bool need_fc = op == "error" || op == "counters";
  if (need_fc || a.contains("tec") || a.contains("rec") || a.contains("epoch")) {
    if (!is_uint(a, "tec", 0xFFFF) || !is_uint(a, "rec", 0xFFFF) || !is_uint(a, "epoch", UINT64_MAX))
      bad("'tec', 'rec' (0..65535) and 'epoch' must be given together as non-negative integers");
    fc = {true, a["tec"].get<uint32_t>(), a["rec"].get<uint32_t>(), a["epoch"].get<uint64_t>()};
  }
  // counters of an older epoch (sent before an MCR.RESET) are dropped
  const auto apply_fc = [this, fc] {
    if (fc.has && fc.epoch == epoch_) set_counters(fc.tec, fc.rec);
  };
  if (need_fc) {
    for (const auto& [k, v] : a.items())
      if (k != "tec" && k != "rec" && k != "epoch" && !(op == "error" && k == "lec")) bad("unknown key '" + k + "'");
  }
  if (op == "error") {
    uint32_t lec = 0;
    if (a.contains("lec") && a["lec"].is_string())
      for (uint32_t i = 1; i < 7; ++i)
        if (a["lec"].get<std::string>() == kLecNames[i]) lec = i;
    if (!lec) bad("'lec' must be one of stuff, form, ack, bit_recessive, bit_dominant, crc");
    clock_.schedule_at(t_ns, [this, fc, lec] {
      if (fc.epoch == epoch_) {  // a receive error from before an MCR.RESET changes nothing
        set_counters(fc.tec, fc.rec);
        set_lec(lec);
      }
      sync();
    });
    return;
  }
  if (op == "counters") {
    clock_.schedule_at(t_ns, [this, apply_fc] {
      apply_fc();
      sync();
    });
    return;
  }
  if (op == "tx_result") {
    if (!is_uint(a, "mailbox", 2))
      bad("'mailbox' must be 0, 1 or 2");
    if (!is_uint(a, "seq", UINT64_MAX)) bad("'seq' must be a non-negative integer");
    if (!is_bool(a, "ok")) bad("'ok' must be a boolean");
    const bool okv = a["ok"].get<bool>();
    const bool arb = is_bool(a, "arb_lost") && a["arb_lost"].get<bool>();
    const bool abt = is_bool(a, "aborted") && a["aborted"].get<bool>();
    for (const char* k : {"arb_lost", "aborted"})
      if (a.contains(k) && !a[k].is_boolean()) bad(std::string("'") + k + "' must be a boolean");
    uint32_t lec = 0;
    if (a.contains("error") && !a["error"].is_null()) {
      if (!a["error"].is_string()) bad("'error' must be null or a string");
      const std::string e = a["error"].get<std::string>();
      for (uint32_t i = 1; i < 7; ++i)
        if (e == kLecNames[i]) lec = i;
      if (!lec) bad("unknown 'error' \"" + e + "\" (stuff, form, ack, bit_recessive, bit_dominant, crc)");
    }
    if (okv + arb + abt + (lec != 0) != 1) bad("exactly one of ok, arb_lost, aborted, error must be set");
    const unsigned m = a["mailbox"].get<unsigned>();
    const uint64_t seq = a["seq"].get<uint64_t>();
    const int outcome = okv ? 0 : arb ? 1 : lec ? 2 : 3;
    clock_.schedule_at(t_ns, [this, apply_fc, m, seq, outcome, lec] {
      apply_fc();  // even for a stale result: the engine's counters are authoritative
      on_tx_result(m, seq, outcome, lec);
      sync();  // on_tx_result returns early on a stale result: ERRI / SCE from the counters
    });
    return;
  }
  if (op == "rx") {
    Frame f;
    if (!is_bool(a, "ext") || !is_bool(a, "rtr")) bad("'ext' and 'rtr' must be booleans");
    f.ext = a["ext"].get<bool>();
    f.rtr = a["rtr"].get<bool>();
    if (!is_uint(a, "id", f.ext ? 0x1FFFFFFFu : 0x7FFu))
      bad(std::string("'id' must be a ") + (f.ext ? "29" : "11") + "-bit identifier");
    f.id = a["id"].get<uint32_t>();
    if (!is_uint(a, "dlc", 8)) bad("'dlc' must be 0..8");
    f.dlc = a["dlc"].get<uint8_t>();
    if (!a.contains("data") || !a["data"].is_string()) bad("'data' must be a hex string");
    const std::string s = a["data"].get<std::string>();
    if (s.size() != (f.rtr ? 0u : 2u * f.dlc)) bad("'data' must hold dlc bytes (none for a remote frame)");
    for (size_t i = 0; i < s.size(); i += 2) {
      const int hi = hexval(s[i]), lo = hexval(s[i + 1]);
      if (hi < 0 || lo < 0) bad("'data' is not hex: " + s);
      f.data[i / 2] = static_cast<uint8_t>(hi << 4 | lo);
    }
    clock_.schedule_at(t_ns, [this, apply_fc, f] {
      apply_fc();  // also when the frame is dropped (mode, filters): it was received on the bus
      on_rx(f);
      sync();  // on_rx returns early outside normal mode: ERRI / SCE from the counters
    });
    return;
  }
  throw UnknownOp(op);
}

}  // namespace stest::stm32l4
