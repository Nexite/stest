// Cortex-M core model: NVIC/SCB/SysTick register blocks + exception dispatcher.
// See stest/cortexm.hpp for the dispatch model.
#include "stest/cortexm.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>
#include "stest/cortexm_c.h"
#include "stest/mmio.h"
#include "stest/peripheral.hpp"
#include "stest/runtime.hpp"

namespace stest {
namespace {

Cortexm* g_installed = nullptr;
void delivery_hook() {
  if (g_installed) g_installed->deliver();
}

constexpr uint64_t kNone = UINT64_MAX;
constexpr int kExcSVCall = 11, kExcDebugMon = 12, kExcPendSV = 14, kExcSysTick = 15;

uint32_t lane_mask(uint32_t off, unsigned width) {
  uint32_t m = width >= 4 ? 0xFFFFFFFFu : ((1u << (width * 8)) - 1u);
  return m << ((off & 3u) * 8);
}

// Word-granular register block: sub-word accesses act on the byte lanes of the
// containing aligned word (CMSIS uses byte access for NVIC->IP[] and SCB->SHP[]).
struct CoreBlock : Peripheral {
  // off is word aligned; mask selects the written byte lanes (v is pre-shifted).
  virtual uint32_t rd(uint32_t off) = 0;
  virtual void wr(uint32_t off, uint32_t v, uint32_t mask) = 0;

  uint32_t read(uint32_t off, unsigned width) override {
    if ((off & 3u) + width > 4) throw UnmodelledRegister(off, width, false);
    return (rd(off & ~3u) & lane_mask(off, width)) >> ((off & 3u) * 8);
  }
  void write(uint32_t off, unsigned width, uint32_t v) override {
    if ((off & 3u) + width > 4) throw UnmodelledRegister(off, width, true);
    uint32_t m = lane_mask(off, width);
    wr(off & ~3u, (v << ((off & 3u) * 8)) & m, m);
  }
};

}  // namespace

struct Cortexm::Impl {
  // ---- register blocks ----
  struct Nvic : CoreBlock {
    Impl& c;
    explicit Nvic(Impl& i) : c(i) {}
    std::string_view name() const override { return "NVIC"; }
    uint32_t bits(const std::vector<uint8_t>& v, uint32_t word) const {
      uint32_t r = 0;
      for (uint32_t b = 0; b < 32; ++b) {
        size_t exc = 16 + word * 32 + b;
        if (exc < c.n_exc && v[exc]) r |= 1u << b;
      }
      return r;
    }
    template <class F> void each(uint32_t word, uint32_t v, F f) {
      for (uint32_t b = 0; b < 32; ++b) {
        size_t exc = 16 + word * 32 + b;
        if ((v >> b & 1u) && exc < c.n_exc) f(static_cast<int>(exc));
      }
    }
    uint32_t rd(uint32_t off) override {
      uint32_t w = (off & 0x7Fu) / 4;
      if (off < 0x020) return bits(c.en, w);                       // ISER
      if (off >= 0x080 && off < 0x0A0) return bits(c.en, w);       // ICER
      if (off >= 0x100 && off < 0x120) return bits(c.pend, w);     // ISPR
      if (off >= 0x180 && off < 0x1A0) return bits(c.pend, w);     // ICPR
      if (off >= 0x200 && off < 0x220) return bits(c.act, w);      // IABR
      if (off >= 0x300 && off < 0x3F0) {                           // IPR
        uint32_t r = 0;
        for (uint32_t b = 0; b < 4; ++b) {
          size_t exc = 16 + (off - 0x300) + b;
          if (exc < c.n_exc) r |= uint32_t(c.prio[exc]) << (8 * b);
        }
        return r;
      }
      throw UnmodelledRegister(off, 4, false);
    }
    void wr(uint32_t off, uint32_t v, uint32_t mask) override {
      uint32_t w = (off & 0x7Fu) / 4;
      if (off < 0x020) each(w, v, [&](int e) { c.en[e] = 1; });
      else if (off >= 0x080 && off < 0x0A0) each(w, v, [&](int e) { c.en[e] = 0; });
      else if (off >= 0x100 && off < 0x120) each(w, v, [&](int e) { c.set_pend(e, true); });
      else if (off >= 0x180 && off < 0x1A0) each(w, v, [&](int e) { c.set_pend(e, false); });
      else if (off >= 0x200 && off < 0x220) {}  // IABR is read-only
      else if (off >= 0x300 && off < 0x3F0) {
        for (uint32_t b = 0; b < 4; ++b) {
          size_t exc = 16 + (off - 0x300) + b;
          if ((mask >> (8 * b) & 0xFFu) && exc < c.n_exc)
            c.prio[exc] = static_cast<uint8_t>(v >> (8 * b)) & c.prio_mask;
        }
      } else {
        throw UnmodelledRegister(off, 4, true);
      }
    }
  };

  struct Scb : CoreBlock {
    Impl& c;
    explicit Scb(Impl& i) : c(i) {}
    std::string_view name() const override { return "SCB"; }
    static bool shpr_exc(int exc) {
      return exc == 4 || exc == 5 || exc == 6 || exc == kExcSVCall || exc == kExcDebugMon ||
             exc == kExcPendSV || exc == kExcSysTick;
    }
    uint32_t rd(uint32_t off) override {
      switch (off) {
        case 0x00: return 0x410FC241u;  // CPUID: Cortex-M4 r0p1
        case 0x04: {                    // ICSR
          uint32_t r = c.ipsr() & 0x1FFu;
          if (c.stack.size() == 1) r |= 1u << 11;  // RETTOBASE
          r |= uint32_t(c.highest_pending() & 0x1FF) << 12;
          for (size_t e = 16; e < c.n_exc; ++e)
            if (c.pend[e]) { r |= 1u << 22; break; }  // ISRPENDING
          if (c.pend[kExcSysTick]) r |= 1u << 26;
          if (c.pend[kExcPendSV]) r |= 1u << 28;
          return r;
        }
        case 0x08: return c.vtor;
        case 0x0C: return 0xFA050000u | (c.prigroup << 8);
        case 0x10: return c.scr;
        case 0x14: return c.ccr;
        case 0x18: case 0x1C: case 0x20: {  // SHPR1-3
          uint32_t r = 0;
          for (int b = 0; b < 4; ++b) {
            int exc = 4 + int(off - 0x18) + b;
            if (shpr_exc(exc)) r |= uint32_t(c.prio[exc]) << (8 * b);
          }
          return r;
        }
        case 0x24: return c.shcsr;
        case 0x28: case 0x2C: return 0;  // CFSR, HFSR: no faults are ever recorded
        case 0x88: return c.cpacr;
      }
      throw UnmodelledRegister(off, 4, false);
    }
    void wr(uint32_t off, uint32_t v, uint32_t mask) override {
      auto merge = [&](uint32_t& reg, uint32_t rw) { reg = (reg & ~(mask & rw)) | (v & mask & rw); };
      switch (off) {
        case 0x00: return;  // CPUID read-only
        case 0x04:
          if (v & (1u << 31)) throw std::runtime_error("ICSR.NMIPENDSET (NMI) not modelled");
          if (v & (1u << 28)) c.set_pend(kExcPendSV, true);
          if (v & (1u << 27)) c.set_pend(kExcPendSV, false);
          if (v & (1u << 26)) c.set_pend(kExcSysTick, true);
          if (v & (1u << 25)) c.set_pend(kExcSysTick, false);
          return;
        case 0x08: merge(c.vtor, 0xFFFFFF80u); return;
        case 0x0C:
          if (mask != 0xFFFFFFFFu || (v >> 16) != 0x05FAu) return;  // needs VECTKEY
          if (v & 4u) throw std::runtime_error("AIRCR.SYSRESETREQ (system reset) not modelled");
          if (v & 3u) throw std::runtime_error("AIRCR.VECTCLRACTIVE/VECTRESET not modelled");
          c.prigroup = (v >> 8) & 7u;
          return;
        case 0x10: merge(c.scr, 0x16u); return;
        case 0x14: merge(c.ccr, 0x31Bu); return;
        case 0x18: case 0x1C: case 0x20:
          for (int b = 0; b < 4; ++b) {
            int exc = 4 + int(off - 0x18) + b;
            if ((mask >> (8 * b) & 0xFFu) && shpr_exc(exc))
              c.prio[exc] = static_cast<uint8_t>(v >> (8 * b)) & c.prio_mask;
          }
          return;
        case 0x24: merge(c.shcsr, 0x00070000u); return;
        case 0x28: case 0x2C: return;  // W1C of always-zero status
        case 0x88: merge(c.cpacr, 0x00F00000u); return;
      }
      throw UnmodelledRegister(off, 4, true);
    }
  };

  struct SysTick : CoreBlock {
    Impl& c;
    explicit SysTick(Impl& i) : c(i) {}
    std::string_view name() const override { return "SysTick"; }
    uint32_t rd(uint32_t off) override {
      switch (off) {
        case 0x0: {  // CSR: reading clears COUNTFLAG
          c.st_sync();
          uint32_t r = c.st_ctrl | (c.st_countflag ? 1u << 16 : 0u);
          c.st_countflag = false;
          return r;
        }
        case 0x4: return c.st_load;
        case 0x8: c.st_sync(); return c.st_value_now();
        case 0xC:  // CALIB: SKEW, TENMS unknown; NOREF unless an external ref is configured
          return (c.ext_ref ? 0u : 0x80000000u) | 0x40000000u;
      }
      throw UnmodelledRegister(off, 4, false);
    }
    void wr(uint32_t off, uint32_t v, uint32_t mask) override {
      switch (off) {
        case 0x0: c.st_write_ctrl(((c.st_ctrl & ~mask) | (v & mask)) & 7u); return;
        case 0x4: c.st_write_load(((c.st_load & ~mask) | (v & mask)) & 0x00FFFFFFu); return;
        case 0x8: c.st_write_val(); return;
        case 0xC: return;  // CALIB read-only
      }
      throw UnmodelledRegister(off, 4, true);
    }
  };

  Impl(Bus& b, Clock& k, unsigned bits, const IrqHandler* t, size_t n)
      : bus(b), clock(k), prio_bits(bits), prio_mask(static_cast<uint8_t>(0xFFu << (8 - bits))),
        table(t), n_exc(n), en(n, 0), pend(n, 0), act(n, 0), prio(n, 0),
        nvic(*this), scb(*this), systick(*this) {}

  Bus& bus;
  Clock& clock;
  unsigned prio_bits;
  uint8_t prio_mask;
  const IrqHandler* table;
  size_t n_exc;
  std::vector<uint8_t> en, pend, act, prio;  // indexed by exception number
  size_t n_pend = 0;
  std::vector<int> stack;                    // active exceptions, innermost last
  bool primask = false, faultmask = false;
  uint32_t basepri = 0, prigroup = 0;
  uint64_t quantum_end = UINT64_MAX;
  uint32_t vtor = 0, scr = 0, ccr = 0x200, shcsr = 0, cpacr = 0;

  CoreClockGetter getter = nullptr;
  uint32_t ext_div = 1;
  bool ext_ref = false;
  QuantumHook quantum_hook = nullptr;
  uint32_t st_ctrl = 0, st_load = 0;
  bool st_countflag = false;
  // Counter state: running -> value st_val at tick boundary st_t0, counting at st_freq;
  // stopped -> st_val is the held value.
  uint32_t st_val = 0;
  uint64_t st_t0 = 0, st_freq = 0, st_next_wrap = kNone;  // wrap = tick index from st_t0
  EventId st_event = 0;
  bool st_has_event = false;

  Nvic nvic;
  Scb scb;
  SysTick systick;

  // ---- dispatcher ----
  int exc_of(int irqn) const {
    if (irqn >= 0 && size_t(irqn) + 16 < n_exc) return irqn + 16;
    if (irqn == kSVCall_IRQn || irqn == kPendSV_IRQn || irqn == kSysTick_IRQn) return irqn + 16;
    throw std::out_of_range("IRQn " + std::to_string(irqn) + " not supported by this core");
  }
  bool exc_enabled(size_t exc) const {
    return exc >= 16 ? en[exc] != 0 : (exc == kExcSVCall || exc == kExcPendSV || exc == kExcSysTick);
  }
  void set_pend(int exc, bool on) {
    if (pend[exc] == on) return;
    pend[exc] = on;
    if (on) ++n_pend; else --n_pend;
  }
  int group(uint32_t p) const { return int(p & ~((2u << prigroup) - 1u) & 0xFFu); }
  int exec_priority(bool with_primask) const {
    if (faultmask) return -1;
    int ep = 256;
    for (int e : stack) ep = std::min(ep, group(prio[e]));
    if (basepri) ep = std::min(ep, group(basepri));
    if (with_primask && primask) ep = std::min(ep, 0);
    return ep;
  }
  // Highest-priority pending enabled exception that would preempt; -1 if none.
  int select(bool with_primask = true) const {
    if (n_pend == 0) return -1;
    int ep = exec_priority(with_primask), best = -1;
    for (size_t e = 1; e < n_exc; ++e) {
      if (!pend[e] || !exc_enabled(e) || group(prio[e]) >= ep) continue;
      if (best < 0 || prio[e] < prio[best]) best = int(e);
    }
    return best;
  }
  int highest_pending() const {  // VECTPENDING: ignores masks
    int best = 0;
    for (size_t e = 1; e < n_exc; ++e)
      if (pend[e] && exc_enabled(e) && (best == 0 || prio[e] < prio[best])) best = int(e);
    return best;
  }
  uint32_t ipsr() const { return stack.empty() ? 0u : uint32_t(stack.back()); }
  void run(int exc) {
    if (act[exc]) throw std::logic_error("exception " + std::to_string(exc) + " already active");
    IrqHandler h = table[exc];
    if (!h) throw std::runtime_error("no handler for exception " + std::to_string(exc));
    set_pend(exc, false);
    act[exc] = 1;
    stack.push_back(exc);
    poll_streak_break();  // taking an exception ends any poll streak
    h();
    stack.pop_back();
    act[exc] = 0;
  }
  void deliver() {
    for (int e; (e = select()) >= 0;) run(e);
  }

  // ---- SysTick ----
  uint64_t st_freq_for(uint32_t ctrl) const {
    if (!getter)
      throw std::runtime_error("SysTick enabled but no core clock getter registered "
                               "(Cortexm::set_core_clock_getter)");
    uint64_t f = getter();
    if (!(ctrl & 4u)) f /= ext_div;  // CLKSOURCE=0: external reference
    if (f == 0) throw std::runtime_error("SysTick clock is 0 Hz (SystemCoreClock not set?)");
    return f;
  }
  bool st_running() const { return st_ctrl & 1u; }
  uint64_t st_ticks_at(uint64_t t) const {
    return uint64_t((unsigned __int128)(t - st_t0) * st_freq / 1'000'000'000u);
  }
  uint64_t st_time_of(uint64_t n) const {  // time of tick n (ceil)
    unsigned __int128 num = (unsigned __int128)n * 1'000'000'000u;
    return st_t0 + uint64_t((num + st_freq - 1) / st_freq);
  }
  uint32_t st_value_at(uint64_t n) const {
    if (n < st_val) return uint32_t(st_val - n);
    if (st_load == 0) return 0;
    uint64_t r = (n - st_val) % (uint64_t(st_load) + 1);
    return r == 0 ? 0u : uint32_t(st_load + 1 - r);
  }
  uint32_t st_value_now() const {
    return st_running() ? st_value_at(st_ticks_at(clock.now_ns())) : st_val;
  }
  uint64_t st_first_wrap() const {
    if (st_val > 0) return st_val;
    return st_load ? uint64_t(st_load) + 1 : kNone;
  }
  // Freeze the count at the last tick boundary <= now (keeps tick phase).
  void st_reanchor() {
    uint64_t n = st_ticks_at(clock.now_ns());
    st_val = st_value_at(n);
    st_t0 = st_time_of(n);
  }
  bool st_catch_up() {  // process wraps that are due; true if any
    bool any = false;
    while (st_running() && st_next_wrap != kNone && st_time_of(st_next_wrap) <= clock.now_ns()) {
      any = true;
      st_countflag = true;
      if (st_ctrl & 2u) set_pend(kExcSysTick, true);
      st_next_wrap = st_load ? st_next_wrap + st_load + 1 : kNone;
    }
    return any;
  }
  void st_reschedule() {
    if (st_has_event) clock.cancel(st_event);
    st_has_event = false;
    if (!st_running() || st_next_wrap == kNone) return;
    st_event = clock.schedule_at(st_time_of(st_next_wrap), [this] {
      st_has_event = false;
      st_sync();
      if (!st_has_event) st_reschedule();
    });
    st_has_event = true;
  }
  // Bring the counter up to now; follows core clock changes.
  void st_sync() {
    if (!st_running()) return;
    bool changed = st_catch_up();
    uint64_t f = st_freq_for(st_ctrl);
    if (f != st_freq) {
      st_reanchor();
      st_freq = f;
      st_next_wrap = st_first_wrap();
      st_catch_up();
      changed = true;
    }
    if (changed || !st_has_event) st_reschedule();
  }
  void st_write_ctrl(uint32_t ctrl) {
    st_sync();
    bool was = st_running(), now_on = ctrl & 1u;
    bool restart = now_on && (!was || ((ctrl ^ st_ctrl) & 4u));
    uint64_t f = now_on ? st_freq_for(ctrl) : 0;  // throws before any state change
    if (was && (!now_on || restart)) st_reanchor();
    st_ctrl = ctrl;
    if (restart) {
      if (!was) st_t0 = clock.now_ns();
      st_freq = f;
      st_next_wrap = st_first_wrap();
    }
    st_reschedule();
  }
  void st_write_load(uint32_t load) {
    st_sync();
    if (st_running()) st_reanchor();
    st_load = load;
    if (st_running()) st_next_wrap = st_first_wrap();
    st_reschedule();
  }
  void st_write_val() {
    st_sync();
    st_val = 0;
    st_countflag = false;
    if (st_running()) {
      st_t0 = clock.now_ns();
      st_next_wrap = st_first_wrap();
    }
    st_reschedule();
  }
};

Cortexm::Cortexm(Bus& bus, Clock& clock, unsigned prio_bits, const IrqHandler* irq_table,
                 size_t n_irqs) {
  if (prio_bits < 2 || prio_bits > 8) throw std::invalid_argument("Cortexm: prio_bits must be 2..8");
  if (!irq_table) throw std::invalid_argument("Cortexm: null irq_table");
  if (n_irqs < 16 || n_irqs > 16 + 240) throw std::invalid_argument("Cortexm: n_irqs must be 16..256");
  impl_ = std::make_unique<Impl>(bus, clock, prio_bits, irq_table, n_irqs);
  bus.map(kSysTickBase, kSysTickSize, &impl_->systick);
  try {
    bus.map(kNvicBase, kNvicSize, &impl_->nvic);
    bus.map(kScbBase, kScbSize, &impl_->scb);
  } catch (...) {
    bus.unmap(&impl_->systick);
    bus.unmap(&impl_->nvic);
    throw;
  }
}

Cortexm::~Cortexm() {
  if (g_installed == this) {
    g_installed = nullptr;
    set_delivery_hook(nullptr);
  }
  if (impl_->st_has_event) impl_->clock.cancel(impl_->st_event);
  impl_->bus.unmap(&impl_->systick);
  impl_->bus.unmap(&impl_->nvic);
  impl_->bus.unmap(&impl_->scb);
}

void Cortexm::set_pending(int irqn) { impl_->set_pend(impl_->exc_of(irqn), true); }
void Cortexm::clear_pending(int irqn) { impl_->set_pend(impl_->exc_of(irqn), false); }
bool Cortexm::pending(int irqn) const { return impl_->pend[impl_->exc_of(irqn)] != 0; }
bool Cortexm::active(int irqn) const { return impl_->act[impl_->exc_of(irqn)] != 0; }
bool Cortexm::enabled(int irqn) const { return impl_->exc_enabled(impl_->exc_of(irqn)); }
uint8_t Cortexm::priority(int irqn) const { return impl_->prio[impl_->exc_of(irqn)]; }

void Cortexm::deliver() { impl_->deliver(); }

void Cortexm::set_primask(bool on) {
  impl_->primask = on;
  if (!on) impl_->deliver();
}
bool Cortexm::primask() const { return impl_->primask; }
void Cortexm::set_basepri(uint32_t v) {
  uint32_t old = impl_->basepri;
  impl_->basepri = v & impl_->prio_mask;
  if (impl_->basepri == 0 || (old != 0 && impl_->basepri > old)) impl_->deliver();
}
void Cortexm::set_basepri_max(uint32_t v) {
  v &= impl_->prio_mask;
  if (v != 0 && (impl_->basepri == 0 || v < impl_->basepri)) impl_->basepri = v;
}
uint32_t Cortexm::basepri() const { return impl_->basepri; }
void Cortexm::set_faultmask(bool on) {
  impl_->faultmask = on;
  if (!on) impl_->deliver();
}
bool Cortexm::faultmask() const { return impl_->faultmask; }
uint32_t Cortexm::ipsr() const { return impl_->ipsr(); }

void Cortexm::wfi() {
  Impl& c = *impl_;
  c.clock.run_due();
  if (c.select(/*with_primask=*/false) >= 0) {  // wake condition ignores PRIMASK
    c.deliver();
    return;
  }
  uint64_t target = std::min(c.clock.next_event_ns(), c.quantum_end);
  if (target == UINT64_MAX)
    throw std::runtime_error("wfi: nothing can wake the core (no pending exception, "
                             "no clock event, no quantum end)");
  uint64_t now = c.clock.now_ns();
  if (target > now) c.clock.advance(target - now);
  c.clock.run_due();
  c.deliver();
  if (c.quantum_hook && c.clock.now_ns() >= c.quantum_end) c.quantum_hook();
}
void Cortexm::set_quantum_hook(QuantumHook h) { impl_->quantum_hook = h; }
void Cortexm::set_quantum_end(uint64_t t_ns) { impl_->quantum_end = t_ns; }
uint64_t Cortexm::quantum_end() const { return impl_->quantum_end; }

void Cortexm::set_core_clock_getter(CoreClockGetter g) {
  impl_->getter = g;
  impl_->st_sync();
}
void Cortexm::set_systick_external_divider(uint32_t div) {
  if (div == 0) throw std::invalid_argument("SysTick external divider must be >= 1");
  impl_->ext_div = div;
  impl_->ext_ref = true;
  impl_->st_sync();
}

void Cortexm::install() {
  g_installed = this;
  set_delivery_hook(&delivery_hook);
}
Cortexm* Cortexm::installed() { return g_installed; }

}  // namespace stest

// ---- C API for the host CMSIS shim ----
namespace {
template <class F> auto guarded(const char* what, F f) -> decltype(f(*stest::Cortexm::installed())) {
  stest::Cortexm* c = stest::Cortexm::installed();
  if (!c) stest_rt_abort("no Cortex-M core installed (%s)", what);
  try {
    return f(*c);
  } catch (const std::exception& e) {
    stest_rt_abort("exception in %s: %s", what, e.what());
  } catch (...) {
    stest_rt_abort("unknown exception in %s", what);
  }
}
}  // namespace

extern "C" {
void stest_rt_wfi(void) { guarded("stest_rt_wfi", [](stest::Cortexm& c) { c.wfi(); }); }
void stest_rt_enable_irq(void) {
  guarded("stest_rt_enable_irq", [](stest::Cortexm& c) { c.set_primask(false); });
}
void stest_rt_disable_irq(void) {
  guarded("stest_rt_disable_irq", [](stest::Cortexm& c) { c.set_primask(true); });
}
uint32_t stest_rt_get_primask(void) {
  return guarded("stest_rt_get_primask", [](stest::Cortexm& c) { return uint32_t(c.primask()); });
}
void stest_rt_set_primask(uint32_t v) {
  guarded("stest_rt_set_primask", [v](stest::Cortexm& c) { c.set_primask(v & 1u); });
}
uint32_t stest_rt_get_basepri(void) {
  return guarded("stest_rt_get_basepri", [](stest::Cortexm& c) { return c.basepri(); });
}
void stest_rt_set_basepri(uint32_t v) {
  guarded("stest_rt_set_basepri", [v](stest::Cortexm& c) { c.set_basepri(v); });
}
void stest_rt_set_basepri_max(uint32_t v) {
  guarded("stest_rt_set_basepri_max", [v](stest::Cortexm& c) { c.set_basepri_max(v); });
}
uint32_t stest_rt_get_faultmask(void) {
  return guarded("stest_rt_get_faultmask", [](stest::Cortexm& c) { return uint32_t(c.faultmask()); });
}
void stest_rt_set_faultmask(uint32_t v) {
  guarded("stest_rt_set_faultmask", [v](stest::Cortexm& c) { c.set_faultmask(v & 1u); });
}
uint32_t stest_rt_get_ipsr(void) {
  return guarded("stest_rt_get_ipsr", [](stest::Cortexm& c) { return c.ipsr(); });
}
}
