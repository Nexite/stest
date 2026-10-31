#include "stm32l4/models/tim.hpp"
#include <algorithm>
#include <stdexcept>
#include "stest/cortexm.hpp"

namespace stest::stm32l4 {
namespace {
enum : uint32_t {
  CR1 = 0x00, CR2 = 0x04, SMCR = 0x08, DIER = 0x0C, SR = 0x10, EGR = 0x14, CCMR1 = 0x18,
  CCMR2 = 0x1C, CCER = 0x20, CNT = 0x24, PSC = 0x28, ARR = 0x2C, RCR = 0x30, CCR1 = 0x34,
  CCR4 = 0x40, BDTR = 0x44, DCR = 0x48, DMAR = 0x4C, CCMR3 = 0x54, CCR5 = 0x58, CCR6 = 0x5C,
};
// CR1 (TIM_CR1_*)
constexpr uint32_t CEN = 1u << 0, UDIS = 1u << 1, URS = 1u << 2, OPM = 1u << 3, DIR = 1u << 4,
                   CMS = 3u << 5, ARPE = 1u << 7, UIFREMAP = 1u << 11;
// SR (TIM_SR_*) / DIER (TIM_DIER_*) / EGR (TIM_EGR_*)
constexpr uint32_t UIF = 1u << 0, COMIF = 1u << 5, TIF = 1u << 6, BIF = 1u << 7, B2IF = 1u << 8,
                   SBIF = 1u << 13;
constexpr uint32_t BIE = 1u << 7, DMA_EN = 0x7F00;  // UDE..TDE
constexpr uint32_t UG = 1u << 0, COMG = 1u << 5, TG = 1u << 6, BG = 1u << 7, B2G = 1u << 8;
// BDTR (TIM_BDTR_*)
constexpr uint32_t BKE = 1u << 12, AOE = 1u << 14, MOE = 1u << 15, BK2E = 1u << 24;
constexpr uint32_t CCPC = 1u << 0;

const char* const kOcModes[8] = {"frozen",         "active_on_match", "inactive_on_match",
                                 "toggle",         "force_inactive",  "force_active",
                                 "pwm1",           "pwm2"};
}  // namespace

Tim::Tim(std::string name, const nlohmann::json& params, ModelContext& ctx)
    : name_(std::move(name)), ctx_(ctx), clock_(ctx.clock()) {
  const std::string kind = params.value("kind", std::string{});
  if (kind == "basic") kind_ = Kind::Basic;
  else if (kind == "general") kind_ = Kind::General;
  else if (kind == "advanced") kind_ = Kind::Advanced;
  else throw std::invalid_argument(name_ + ": tim params need kind = \"basic\"|\"general\"|\"advanced\"");
  width_ = params.value("width", 16u);
  if (width_ != 16 && width_ != 32) throw std::invalid_argument(name_ + ": tim width must be 16 or 32");
  max_ = width_ == 32 ? 0xFFFFFFFFu : 0xFFFFu;
  const std::string bus = params.value("bus", std::string{});
  if (bus != "apb1" && bus != "apb2") throw std::invalid_argument(name_ + ": tim params need bus = \"apb1\"|\"apb2\"");
  apb2_ = bus == "apb2";
  unsigned def_ch = kind_ == Kind::Basic ? 0 : 4;
  nch_ = params.value("channels", def_ch);
  ncomp_ = params.value("complementary", kind_ == Kind::Advanced ? 3u : 0u);
  up_only_ = kind_ == Kind::Basic || params.value("up_only", false);
  if (nch_ > 4 || ncomp_ > nch_ || (kind_ == Kind::Basic && nch_ != 0))
    throw std::invalid_argument(name_ + ": bad tim channels/complementary");
  has_ch56_ = kind_ == Kind::Advanced;
  has_smcr_ = kind_ != Kind::Basic && nch_ >= 2;  // TIM16/17 (one channel) have no SMCR
  if (params.contains("irq")) {
    const auto& irq = params["irq"];
    const char* keys[kNumSrc] = {"update", "cc", "break", "trg_com"};
    for (int s = 0; s < kNumSrc; ++s)
      if (irq.contains(keys[s])) irq_[s] = irq[keys[s]].get<int>();
  }
  if (params.contains("opt"))
    for (const auto& o : params["opt"]) opt_.emplace_back(o.at(0).get<uint32_t>(), o.at(1).get<uint32_t>());

  // Writable bits per variant (TIM_CR1_* ... from stm32l496xx.h; reserved bits read 0).
  const bool adv = kind_ == Kind::Advanced, tim15 = !adv && ncomp_ > 0 && nch_ == 2,
             tim16 = !adv && ncomp_ > 0 && nch_ == 1;
  if (kind_ == Kind::Basic) {
    cr1_m_ = CEN | UDIS | URS | OPM | ARPE | UIFREMAP;
    cr2_m_ = 0x70;            // MMS
    dier_m_ = 0x0101;         // UIE, UDE
    sr_m_ = UIF;
    egr_m_ = UG;
  } else if (adv) {
    cr1_m_ = 0xBFF;
    cr2_m_ = 0x00F57FFD;      // CCPC CCUS CCDS MMS TI1S OIS1..OIS4 OIS5 OIS6 MMS2
    dier_m_ = 0x7FFF;
    sr_m_ = 0x33FFF;          // UIF..CC4IF COMIF TIF BIF B2IF CC1OF..CC4OF SBIF CC5IF CC6IF
    egr_m_ = 0x1FF;
    ccer_m_ = 0x33BFFF;       // CH1-3 E/P/NE/NP, CH4 E/P/NP, CC5E/P, CC6E/P
    bdtr_m_ = 0x03FFFFFF;
    rcr_m_ = 0xFFFF;
  } else if (tim15) {
    cr1_m_ = 0xB8F;           // no DIR/CMS
    cr2_m_ = 0x7FD;           // CCPC CCUS CCDS MMS TI1S OIS1 OIS1N OIS2
    dier_m_ = 0x67E7;
    sr_m_ = 0x6E7;
    egr_m_ = 0xE7;
    ccer_m_ = 0xBF;
    bdtr_m_ = 0xFFFFF;
    rcr_m_ = 0xFF;
  } else if (tim16) {
    cr1_m_ = 0xB8F;
    cr2_m_ = 0x30D;           // CCPC CCUS CCDS OIS1 OIS1N
    dier_m_ = 0x3A3;          // UIE CC1IE COMIE BIE UDE CC1DE
    sr_m_ = 0x2A3;
    egr_m_ = 0xA3;
    ccer_m_ = 0xF;
    bdtr_m_ = 0xFFFFF;
    rcr_m_ = 0xFF;
  } else {                    // TIM2-5
    cr1_m_ = up_only_ ? 0xB8F : 0xBFF;
    cr2_m_ = 0xF8;            // CCDS MMS TI1S
    dier_m_ = 0x5F5F;
    sr_m_ = 0x1E5F;
    egr_m_ = 0x5F;
    ccer_m_ = 0xBBBB;
  }
  if (up_only_) cr1_m_ &= ~(DIR | CMS);

  regs_.declare(CR1, 0, cr1_m_);
  regs_.declare(CR2, 0, cr2_m_);
  if (has_smcr_) regs_.declare(SMCR, 0, 0x0001FFF7);
  else regs_.declare(SMCR, 0, 0);  // reserved: reads 0 (ST's HAL reads it), writes abort
  regs_.declare(DIER, 0, dier_m_);
  regs_.declare(SR, 0, sr_m_);
  regs_.declare_write_only(EGR);
  regs_.declare(CNT, 0, max_);
  regs_.declare(PSC, 0, 0xFFFF);
  regs_.declare(ARR, max_, max_);
  if (nch_ >= 1) {
    uint32_t ccmr = nch_ >= 2 ? 0x0101FFFF : 0x000100FF;
    regs_.declare(CCMR1, 0, ccmr);
    regs_.declare(CCER, 0, ccer_m_);
  }
  if (nch_ >= 3) regs_.declare(CCMR2, 0, 0x0101FFFF);
  for (unsigned ch = 0; ch < nch_; ++ch) regs_.declare(CCR1 + 4 * ch, 0, max_);
  if (has_bdtr()) {
    regs_.declare(RCR, 0, rcr_m_);
    regs_.declare(BDTR, 0, bdtr_m_);
  }
  if (kind_ != Kind::Basic) {
    regs_.declare(DCR, 0, 0x1F1F);
    regs_.declare_write_only(DMAR);
  }
  if (has_ch56_) {
    regs_.declare(CCMR3, 0, 0x0101FCFC);
    regs_.declare(CCR5, 0, 0xE000FFFF);
    regs_.declare(CCR6, 0, 0xFFFF);
  }
  for (auto [off, rst] : opt_) regs_.declare(off, rst, 0xFFFFFFFF);
  arr_a_ = max_;
}

void Tim::connect(ModelContext& ctx) {
  clocks_ = &ctx.clocks();
  ctx.clock_tree_changed().subscribe([this](const ClockTree&) { on_clock_change(); });
}

void Tim::reset() {
  cancel_event();
  regs_.reset();
  psc_a_ = 0;
  arr_a_ = max_;
  rcr_a_ = 0;
  ccr_a_.fill(0);
  running_ = false;
  cnt_ = rep_ = 0;
  anchor_live_ = false;
  down_ = false;
  clk_ = 0;
  update_irqs();
}

// ---------------------------------------------------------------- configuration helpers

void Tim::not_modelled(const std::string& what) const { throw NotModelled(name_, what); }

void Tim::check_write(uint32_t off, uint32_t v) const {
  switch (off) {
    case SMCR:
      if (!has_smcr_) return;  // read-only: RegisterFile aborts
      if (v & 0x10007u) not_modelled("slave mode controller (SMCR.SMS, incl. encoder modes)");
      if (v & (1u << 14)) not_modelled("external clock mode 2 (SMCR.ECE)");
      return;
    case DIER:
      if (v & DMA_EN) not_modelled("DMA requests (DIER.UDE/CCxDE/COMDE/TDE)");
      return;
    case CR2:
      if (kind_ != Kind::Basic && has_bdtr() && (v & CCPC))
        not_modelled("preloaded capture/compare control (CR2.CCPC, commutation)");
      return;
    case CCMR1: case CCMR2: {
      unsigned first = off == CCMR1 ? 0 : 2;
      for (unsigned h = 0; h < 2 && first + h < nch_; ++h) {
        unsigned s = (v >> (8 * h)) & 3u;
        unsigned ocm = ((v >> (8 * h + 4)) & 7u) | (((v >> (16 + 8 * h)) & 1u) << 3);
        std::string ch = std::to_string(first + h + 1);
        if (s) not_modelled("input capture (CCMR CC" + ch + "S=" + std::to_string(s) + ")");
        if (ocm >= 8)
          not_modelled("output compare mode OC" + ch + "M=" + std::to_string(ocm) +
                           " (retriggerable OPM / combined / asymmetric PWM)");
        if (v & (1u << (8 * h + 7))) not_modelled("OCREF clear (CCMR OC" + ch + "CE)");
      }
      return;
    }
    case CCER:
      if (v & ((1u << 16) | (1u << 20))) not_modelled("channels 5/6 (CCER.CC5E/CC6E)");
      return;
    case CCMR3:
      if (v & 0x01017070u) not_modelled("channels 5/6 (CCMR3.OC5M/OC6M)");
      return;
    case CCR5:
      if (v & 0xE0000000u) not_modelled("channels 5/6 (CCR5.GC5Cx)");
      return;
    case BDTR:
      if (v & (BKE | BK2E)) not_modelled("break inputs (BDTR.BKE/BK2E)");
      return;
    default:
      for (auto [o, rst] : opt_)
        if (o == off && v != rst) not_modelled("option register remap / break-input selection");
      return;
  }
}

Tim::Mode Tim::mode_of(uint32_t cr1) const {
  if (up_only_) return Mode::Up;
  if (cr1 & CMS) return Mode::Center;
  return (cr1 & DIR) ? Mode::Down : Mode::Up;
}

uint64_t Tim::timer_clk() const {
  if (!clocks_) return 0;
  uint64_t pclk = apb2_ ? clocks_->pclk2_hz() : clocks_->pclk1_hz();
  return pclk != clocks_->hclk_hz() ? 2 * pclk : pclk;  // x2 when the APB prescaler != 1
}

bool Tim::ocpe(unsigned ch) const {
  uint32_t ccmr = regs_.get(ch < 2 ? CCMR1 : CCMR2);
  return (ccmr >> (8 * (ch & 1u) + 3)) & 1u;
}

void Tim::load_preloads() {
  psc_a_ = regs_.get(PSC);
  arr_a_ = regs_.get(ARR);
  if (has_bdtr()) rcr_a_ = regs_.get(RCR);
  for (unsigned ch = 0; ch < nch_; ++ch) ccr_a_[ch] = ccr_reg(ch);
}

bool Tim::preload_pending() const {
  if (regs_.get(PSC) != psc_a_ || regs_.get(ARR) != arr_a_) return true;
  if (has_bdtr() && regs_.get(RCR) != rcr_a_) return true;
  for (unsigned ch = 0; ch < nch_; ++ch)
    if (ccr_reg(ch) != ccr_a_[ch]) return true;
  return false;
}

bool Tim::aoe_pending() const {
  if (!has_bdtr()) return false;
  uint32_t b = regs_.get(BDTR);
  return (b & AOE) && !(b & MOE);
}

void Tim::apply_aoe() {
  if (aoe_pending()) regs_.set(BDTR, regs_.get(BDTR) | MOE);
}

// ---------------------------------------------------------------- counter position

int64_t Tim::period() const {
  return mode_ == Mode::Center ? 2 * static_cast<int64_t>(arr_a_) : static_cast<int64_t>(arr_a_) + 1;
}

int64_t Tim::q_at(uint64_t t_ns) const {
  if (!moving()) return q0_;
  const i128 t = static_cast<i128>(t_ns) * clk_;
  if (t <= anchor_) return q0_;
  return q0_ + static_cast<int64_t>((t - anchor_) / unit());
}

uint64_t Tim::time_of(int64_t q) const {
  const i128 num = anchor_ + static_cast<i128>(q - q0_) * unit();  // > 0 for q > q0_
  return static_cast<uint64_t>((num + clk_ - 1) / clk_);
}

uint32_t Tim::cnt_of(int64_t q) const {
  const int64_t arr = arr_a_, L = period();
  switch (mode_) {
    case Mode::Up: return static_cast<uint32_t>(q < 0 ? q + static_cast<int64_t>(max_) + 1 : q % L);
    case Mode::Down: return static_cast<uint32_t>(q < 0 ? arr - q : arr - q % L);
    case Mode::Center: {
      if (q < 0) return static_cast<uint32_t>(arr - q);
      int64_t m = q % L;
      return static_cast<uint32_t>(m <= arr ? arr - m : m - arr);
    }
  }
  return 0;
}

// Direction of the next count. Center: q in [0, ARR) of the period counts down (q = 0 is
// the overflow at CNT = ARR), [ARR, 2*ARR) counts up (q = ARR is the underflow at CNT = 0).
bool Tim::down_of(int64_t q) const {
  switch (mode_) {
    case Mode::Up: return false;
    case Mode::Down: return true;
    case Mode::Center: return q < 0 || q % period() < static_cast<int64_t>(arr_a_);
  }
  return false;
}

int64_t Tim::q_from(uint32_t cnt, bool down) const {
  const int64_t arr = arr_a_, c = cnt;
  switch (mode_) {
    case Mode::Up: return c <= arr ? c : c - (static_cast<int64_t>(max_) + 1);  // > ARR: count to max, roll over
    case Mode::Down: return arr - c;                                          // > ARR: count down past it
    case Mode::Center:
      if (c > arr) return arr - c;
      return down ? arr - c : arr + c;
  }
  return 0;
}

// Wrap points (overflow/underflow; each decrements the repetition counter):
//   up: q = 0, L, 2L... (q = 0 only reachable from a CNT > ARR prefix: max -> 0 roll-over)
//   down: q = L, 2L...   center: every multiple of ARR above 0 (over- and underflows)
int64_t Tim::wraps_upto(int64_t q) const {
  switch (mode_) {
    case Mode::Up: return q < 0 ? 0 : q / period() + 1;
    case Mode::Down: return q <= 0 ? 0 : q / period();
    case Mode::Center: return q <= 0 ? 0 : q / static_cast<int64_t>(arr_a_);
  }
  return 0;
}

int64_t Tim::wrap_point(int64_t n) const {
  switch (mode_) {
    case Mode::Up: return (n - 1) * period();
    case Mode::Down: return n * period();
    case Mode::Center: return n * static_cast<int64_t>(arr_a_);
  }
  return kNever;
}

bool Tim::is_wrap(int64_t q) const {
  switch (mode_) {
    case Mode::Up: return q >= 0 && q % period() == 0;
    case Mode::Down: return q > 0 && q % period() == 0;
    case Mode::Center: return q > 0 && q % static_cast<int64_t>(arr_a_) == 0;
  }
  return false;
}

uint32_t Tim::rep_at(int64_t q) const {
  const int64_t w = wraps_upto(q) - wraps_upto(q0_), r0 = r0_, R = rep_reload();
  if (w <= r0) return static_cast<uint32_t>(r0 - w);
  return static_cast<uint32_t>(R - (w - r0 - 1) % (R + 1));
}

int64_t Tim::next_uev(int64_t qf) const { return wrap_point(wraps_upto(qf) + rep_at(qf) + 1); }

namespace {
// Smallest q > qf, q >= lb, q == res (mod p); res in [0, p), lb >= 0.
int64_t next_periodic(int64_t qf, int64_t res, int64_t p, int64_t lb) {
  int64_t start = std::max(qf + 1, lb);
  if (start <= res) return res;
  return res + (start - res + p - 1) / p * p;
}
}  // namespace

int64_t Tim::next_match(unsigned ch, int64_t qf) const {
  const int64_t v = ccr_a_[ch], arr = arr_a_, L = period();
  switch (mode_) {
    case Mode::Up:
      if (v <= arr) return next_periodic(qf, v, L, 0);
      if (v > static_cast<int64_t>(max_)) return kNever;
      return v - (static_cast<int64_t>(max_) + 1) > qf ? v - (static_cast<int64_t>(max_) + 1) : kNever;
    case Mode::Down:
      if (v <= arr) return next_periodic(qf, arr - v, L, 0);
      return arr - v > qf ? arr - v : kNever;
    case Mode::Center: {
      const bool down_ok = cms_ & 1u, up_ok = cms_ & 2u;  // CMS=1 down, 2 up, 3 both
      int64_t best = kNever;
      if (v > arr) return (down_ok && arr - v > qf) ? arr - v : kNever;
      if (down_ok && v < arr) best = std::min(best, next_periodic(qf, arr - v, L, 1));
      if (up_ok && v >= 1) best = std::min(best, next_periodic(qf, (arr + v) % L, L, 1));
      return best;
    }
  }
  return kNever;
}

void Tim::move_anchor(int64_t q) {
  anchor_ += static_cast<i128>(q - q0_) * unit();
  r0_ = rep_at(q);
  q0_ = q;
  anchor_live_ = true;
}

void Tim::freeze() {
  if (moving()) {
    int64_t q = q_at(clock_.now_ns());
    move_anchor(q);
    cnt_ = cnt_of(q);
    down_ = down_of(q);
    rep_ = r0_;
  } else {
    anchor_live_ = false;  // not counting (stopped, ARR = 0, no clock): no tick to resume from
  }
  running_ = false;
}

void Tim::thaw(bool restart) {
  uint32_t cr1 = regs_.get(CR1);
  mode_ = mode_of(cr1);
  cms_ = (cr1 & CMS) >> 5;
  uint64_t clk = timer_clk();
  if (restart || clk_ == 0 || !anchor_live_)  // the blocked interval is not elapsed ticks
    anchor_ = static_cast<i128>(clock_.now_ns()) * clk;
  else if (clk != clk_)
    anchor_ = anchor_ * clk / clk_;  // RCC changed: keep the anchor's time, in the new unit
  clk_ = clk;
  running_ = true;
  anchor_live_ = true;
  r0_ = rep_;
  if (arr_a_ != 0) q0_ = q_from(cnt_, down_);
}

// The RCC changed the clock tree (at clock_.now_ns()). Only a running counter cares: a
// stopped one reads the clock in thaw() when CEN is set again.
void Tim::on_clock_change() {
  if (!running_) return;
  const uint64_t clk = timer_clk();
  if (clk == clk_) return;
  const uint64_t now = clock_.now_ns();
  if (moving() && clk != 0) {
    // Position units are ns x Hz, i.e. kernel clocks x 1e9, so the part of the current
    // tick already counted (r) is the same in both units. Advance the anchor to the last
    // whole tick, then place it so that the remaining unit() - r of that tick runs at the
    // new rate: tick q0_ + 1 lands at now + (unit() - r) / clk. Exact; CNT does not jump.
    const i128 e = static_cast<i128>(now) * clk_ - anchor_;
    const int64_t n = e > 0 ? static_cast<int64_t>(e / unit()) : 0;
    const i128 r = e > 0 ? e % unit() : 0;
    move_anchor(q0_ + n);
    anchor_ = static_cast<i128>(now) * clk - r;
    clk_ = clk;
  } else {
    // Blocked (ARR = 0), clock stopping (clk = 0) or returning (clk_ = 0): hold the
    // counter here; thaw() resumes it from now at the new rate (or keeps it held at 0 Hz).
    freeze();
    thaw(false);
  }
  schedule();
}

// ---------------------------------------------------------------- events and flags

void Tim::set_sr(uint32_t bits) { regs_.set(SR, regs_.get(SR) | (bits & sr_m_)); }

void Tim::do_ug() {
  bool was = running_;
  freeze();
  uint32_t cr1 = regs_.get(CR1);
  if (!(cr1 & UDIS)) {
    load_preloads();
    rep_ = rep_reload();
    if (!(cr1 & URS)) set_sr(UIF);
    apply_aoe();
  }
  Mode m = mode_of(cr1);
  cnt_ = m == Mode::Down ? arr_a_ : 0;  // counter (and prescaler) reinitialised
  down_ = m == Mode::Down;
  if (was) thaw(true);
}

void Tim::on_event() {
  has_ev_ = false;
  const int64_t q = ev_q_;
  uint32_t sr = regs_.get(SR), set = 0;
  for (unsigned ch = 0; ch < nch_; ++ch) {
    uint32_t bit = 2u << ch;
    if (!(sr & bit) && next_match(ch, q - 1) == q) set |= bit;
  }
  set_sr(set);
  uint32_t cr1 = regs_.get(CR1);
  int64_t from = q;  // position of this event in the coordinates schedule_from() uses
  if (!(cr1 & UDIS) && is_wrap(q) && rep_at(q - 1) == 0) {  // counter update event
    const bool overflow = mode_ == Mode::Up ||
                          (mode_ == Mode::Center && q % period() == 0);
    move_anchor(q);
    load_preloads();
    set_sr(UIF);
    apply_aoe();
    rep_ = rep_reload();
    if (mode_ == Mode::Up) { cnt_ = 0; down_ = false; }
    else if (mode_ == Mode::Down) { cnt_ = arr_a_; down_ = true; }
    else if (overflow) { cnt_ = arr_a_; down_ = true; }
    else { cnt_ = 0; down_ = false; }
    running_ = false;
    if (cr1 & OPM) {
      regs_.set(CR1, cr1 & ~CEN);  // one-pulse: the counter stops
    } else {
      thaw(false);  // renumbers positions: this tick (the anchor) is now q0_
      from = q0_;
    }
  }
  update_irqs();
  if (running_) schedule_from(from);
}

void Tim::cancel_event() {
  if (has_ev_) clock_.cancel(ev_);
  has_ev_ = false;
}

void Tim::schedule() {
  if (moving()) schedule_from(q_at(clock_.now_ns()));
  else cancel_event();
}

void Tim::schedule_from(int64_t qf) {
  cancel_event();
  if (!moving()) return;
  const uint32_t sr = regs_.get(SR), cr1 = regs_.get(CR1);
  int64_t best = kNever;
  if (!(cr1 & UDIS) && (!(sr & UIF) || preload_pending() || (cr1 & OPM) || aoe_pending()))
    best = next_uev(qf);
  for (unsigned ch = 0; ch < nch_; ++ch)
    if (!(sr & (2u << ch))) best = std::min(best, next_match(ch, qf));
  if (best == kNever) return;
  ev_q_ = best;
  ev_ = clock_.schedule_at(time_of(best), [this] { on_event(); });
  has_ev_ = true;
}

void Tim::update_irqs() {
  const uint32_t sr = regs_.get(SR), dier = regs_.get(DIER), on = sr & dier;
  const bool lv[kNumSrc] = {
      (on & UIF) != 0,
      (on & 0x1Eu) != 0,
      (sr & (BIF | B2IF | SBIF)) && (dier & BIE),
      (on & (COMIF | TIF)) != 0,
  };
  for (int s = 0; s < kNumSrc; ++s)
    if (irq_[s] >= 0 && lv[s] != level_[s]) {
      level_[s] = lv[s];
      ctx_.set_irq_source(irq_[s], &src_tag_[s], lv[s]);
    }
}

// ---------------------------------------------------------------- bus access

uint32_t Tim::read(uint32_t off, unsigned width) {
  const uint32_t aligned = off & ~3u;
  switch (aligned) {
    case CNT: {
      (void)regs_.read(off, width);  // strictness checks
      uint32_t c = moving() ? cnt_of(q_at(clock_.now_ns())) : cnt_;
      if (regs_.get(CR1) & UIFREMAP) c = (c & 0x7FFFFFFFu) | ((regs_.get(SR) & UIF) << 31);
      return lane_extract(c, off, width);
    }
    case CR1: {
      uint32_t v = regs_.get(CR1);
      (void)regs_.read(off, width);
      if (v & CMS) {  // DIR reports the live direction in center-aligned mode
        bool down = moving() ? down_of(q_at(clock_.now_ns())) : down_;
        v = (v & ~DIR) | (down ? DIR : 0u);
      }
      return lane_extract(v, off, width);
    }
    case DMAR:
      if (kind_ != Kind::Basic) not_modelled("DMA burst access (DMAR)");
      return regs_.read(off, width);
    default:
      return regs_.read(off, width);
  }
}

void Tim::write(uint32_t off, unsigned width, uint32_t v) {
  const uint32_t aligned = off & ~3u;
  uint32_t old = 0;
  try {
    old = regs_.get(aligned);
  } catch (const UnmodelledRegister&) {
    regs_.write(off, width, v);  // undeclared offset: throws the strict write error
    throw;
  }
  const uint32_t full = lane_merge(old, off, width, v);
  check_write(aligned, full);
  switch (aligned) {
    case CR1: {
      const uint32_t old = regs_.get(CR1);
      const bool was = running_;
      freeze();
      regs_.write(off, width, v);
      uint32_t now = regs_.get(CR1);
      if (now & CMS) now = (now & ~DIR) | (old & DIR);  // DIR is read-only in center mode
      regs_.set(CR1, now);
      if (!(now & CMS) && !up_only_) down_ = (now & DIR) != 0;
      if (!(now & ARPE)) arr_a_ = regs_.get(ARR);
      if (now & CEN) thaw(!was);
      schedule();
      return;
    }
    case SR: {
      (void)regs_.read(off, width);  // strictness (declared)
      uint32_t keep = lane_merge(0xFFFFFFFFu, off, width, v);  // rc_w0: written 0 clears
      regs_.set(SR, regs_.get(SR) & keep);
      update_irqs();
      schedule();
      return;
    }
    case EGR: {
      regs_.write(off, width, v);  // write-only: strictness checks
      uint32_t e = lane_merge(0, off, width, v) & egr_m_;
      if (e & UG) do_ug();
      set_sr(e & 0x1Eu);  // CCxG
      if (e & TG) set_sr(TIF);
      if (e & COMG) set_sr(COMIF);
      if (e & (BG | B2G)) {
        set_sr(((e & BG) ? BIF : 0u) | ((e & B2G) ? B2IF : 0u));
        regs_.set(BDTR, regs_.get(BDTR) & ~MOE);  // break: outputs off
      }
      update_irqs();
      schedule();
      return;
    }
    case CNT: {
      regs_.write(off, width, v);
      const bool was = running_;
      freeze();
      cnt_ = lane_merge(cnt_, off, width, v) & max_;
      if (was) thaw(false);
      schedule();
      return;
    }
    case ARR:
      regs_.write(off, width, v);
      if (!(regs_.get(CR1) & ARPE)) {  // not buffered: takes effect now
        const bool was = running_;
        freeze();
        arr_a_ = regs_.get(ARR);
        if (was) thaw(false);
      }
      schedule();
      return;
    case DIER:
      regs_.write(off, width, v);
      update_irqs();
      return;
    case DMAR:
      if (kind_ != Kind::Basic) not_modelled("DMA burst access (DMAR)");
      regs_.write(off, width, v);
      return;
    default:
      regs_.write(off, width, v);
      if (aligned >= CCR1 && aligned <= CCR4 && (aligned - CCR1) / 4 < nch_) {
        unsigned ch = (aligned - CCR1) / 4;
        if (!ocpe(ch)) ccr_a_[ch] = ccr_reg(ch);
      } else if (aligned == CCMR1 || aligned == CCMR2) {
        for (unsigned ch = 0; ch < nch_; ++ch)
          if (!ocpe(ch)) ccr_a_[ch] = ccr_reg(ch);
      }
      schedule();  // PSC/RCR/CCRx/BDTR (AOE) may change what has to be scheduled
      return;
  }
}

// ---------------------------------------------------------------- state report

std::optional<double> Tim::ref_duty(unsigned ch, unsigned ocm) const {
  switch (ocm) {
    case 3: return 0.5;
    case 4: return 0.0;
    case 5: return 1.0;
    case 6: case 7: break;
    default: return std::nullopt;
  }
  const double v = ccr_a_[ch], arr = arr_a_;
  double d;
  switch (mode_of(regs_.get(CR1))) {
    case Mode::Up: d = std::min(v, arr + 1) / (arr + 1); break;
    case Mode::Down: d = std::min(v + 1, arr + 1) / (arr + 1); break;
    default: d = arr == 0 ? (v > 0 ? 1.0 : 0.0) : std::min(v, arr) / arr; break;
  }
  return ocm == 6 ? d : 1.0 - d;
}

double Tim::dead_time_ns(uint64_t clk) const {
  if (!has_bdtr() || clk == 0) return 0.0;
  const uint32_t dtg = regs_.get(BDTR) & 0xFFu, ckd = std::min((regs_.get(CR1) >> 8) & 3u, 2u);
  const double tdts = static_cast<double>(1u << ckd) * 1e9 / static_cast<double>(clk);
  if ((dtg >> 7) == 0) return dtg * tdts;
  if ((dtg >> 6) == 2) return (64 + (dtg & 0x3Fu)) * 2 * tdts;
  if ((dtg >> 5) == 6) return (32 + (dtg & 0x1Fu)) * 8 * tdts;
  return (32 + (dtg & 0x1Fu)) * 16 * tdts;
}

nlohmann::json Tim::call(std::string_view op, const nlohmann::json&) {
  if (op != "state") throw UnknownOp(op);
  using nlohmann::json;
  const uint32_t cr1 = regs_.get(CR1);
  const uint64_t clk = running_ ? clk_ : timer_clk();
  const Mode m = mode_of(cr1);
  const unsigned cms = (cr1 & CMS) >> 5;
  const bool counting = running_ && arr_a_ != 0 && clk != 0;
  const double fcnt = static_cast<double>(clk) / (psc_a_ + 1.0);
  const double period_ticks = m == Mode::Center ? 2.0 * arr_a_ : arr_a_ + 1.0;
  double update_hz = 0.0;
  if (counting && !(cr1 & UDIS)) {
    double per_uev = (m == Mode::Center ? double(arr_a_) : arr_a_ + 1.0) * (rep_reload() + 1.0);
    update_hz = fcnt / per_uev;
  }
  const char* counter = m == Mode::Up ? "up" : m == Mode::Down ? "down"
                        : cms == 1 ? "center1" : cms == 2 ? "center2" : "center3";
  json st = {{"running", running_},
             {"cnt", moving() ? cnt_of(q_at(clock_.now_ns())) : cnt_},
             {"arr", arr_a_},
             {"psc", psc_a_},
             {"clk_hz", clk},
             {"update_hz", update_hz},
             {"counter", counter},
             {"channels", json::array()}};
  if (has_bdtr()) st["rcr"] = rcr_a_;
  const bool moe = !has_bdtr() || (regs_.get(BDTR) & MOE);
  const double dt = dead_time_ns(clk);
  const double period_ns = (clk && fcnt > 0) ? period_ticks / fcnt * 1e9 : 0.0;
  for (unsigned ch = 0; ch < nch_; ++ch) {
    const uint32_t ccmr = regs_.get(ch < 2 ? CCMR1 : CCMR2), h = ch & 1u;
    const unsigned ocm = ((ccmr >> (8 * h + 4)) & 7u) | (((ccmr >> (16 + 8 * h)) & 1u) << 3);
    const uint32_t ccer = regs_.get(CCER) >> (4 * ch);
    const bool e = ccer & 1u, ne = ccer & 4u;
    const std::optional<double> ref = ref_duty(ch, ocm);
    json c = {{"ch", ch + 1},
              {"enabled", e && moe},
              {"mode", ocm < 8 ? kOcModes[ocm] : "unsupported"},
              {"polarity", (ccer & 2u) ? "low" : "high"},
              {"ccr", ccr_a_[ch]},
              {"ref_duty", ref ? json(*ref) : json(nullptr)}};
    json duty = c["ref_duty"], duty_n = ref ? json(1.0 - *ref) : json(nullptr);
    if (ch < ncomp_) {
      if (ref && e && ne && *ref > 0.0 && *ref < 1.0 && period_ns > 0) {
        const double f = dt / period_ns;  // each output loses the dead time once per period
        duty = std::max(0.0, *ref - f);
        duty_n = std::max(0.0, 1.0 - *ref - f);
      } else if (ref && !e && ne) {
        duty_n = *ref;  // CCxE=0, CCxNE=1: OCxN = OCxREF, no dead time (RM0351 table 197)
      }
      c["enabled_n"] = ne && moe;
      c["polarity_n"] = (ccer & 8u) ? "low" : "high";
      c["duty_n"] = duty_n;
      c["dead_time_ns"] = dt;
    }
    c["duty"] = duty;
    st["channels"].push_back(std::move(c));
  }
  return st;
}

}  // namespace stest::stm32l4
