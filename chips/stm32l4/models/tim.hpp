#pragma once
// STM32L4 (L496) timers, RM0351 sections 31 (TIM1/TIM8), 32 (TIM2-5), 33 (TIM15-17) and
// 34 (TIM6/7). Register layout and bits follow ST's stm32l496xx.h (TIM_TypeDef, TIM_*).
//
// One class covers every variant, chosen by params:
//   kind = "basic"    TIM6/7: CR1 CR2 DIER SR EGR CNT PSC ARR; up-counting only, no channels.
//                     SMCR (0x08) is reserved on these timers but ST's HAL reads it
//                     (HAL_TIMEx_MasterConfigSynchronization), so it reads 0 and writes abort.
//          "general"  TIM2-5 (4 channels, defaults) and, with channels = 2 / 1,
//                     complementary = 1, up_only = true: TIM15 / TIM16-17 (which also get RCR
//                     and BDTR; TIM16/17 have no SMCR: reads 0, writes abort).
//          "advanced" TIM1/8: 4 channels (CH1-3 complementary), RCR, BDTR, CCMR3/CCR5/CCR6.
//   width = 16 | 32 (CNT/ARR/CCRx width; TIM2/TIM5 are 32-bit)
//   bus   = "apb1" | "apb2": timer kernel clock = PCLKx, x2 when the APB prescaler != 1
//           (RM0351 6.2.14). Read from the RCC clock tree whenever the timer (re)schedules.
//   irq   = {"update": n, "cc": n, "break": n, "trg_com": n} (missing key: no vector).
//           Each source is a flag-driven level (UIF&UIE, CCxIF&CCxIE, (BIF|B2IF|SBIF)&BIE,
//           (TIF&TIE)|(COMIF&COMIE)) driven through ModelContext::set_irq_source, so shared
//           vectors (TIM1_UP_TIM16, ...) see the OR of all their sources.
//   opt   = [[offset, reset], ...]: option registers (OR1/OR2/OR3). Any write that changes
//           them aborts (input remapping / break-input selection are not modelled).
//
// Counter: CNT is derived from virtual time (no per-tick events). Position q counts timer
// ticks (tick = (PSC+1) kernel clocks) from an exact anchor; up: CNT = q mod (ARR+1),
// down: ARR - q mod (ARR+1), center-aligned: triangle 0..ARR..0 of period 2*ARR. Update
// events (UEV): overflow (up), underflow (down), both (center); gated by the repetition
// counter (RCR, advanced/TIM15-17: UEV every RCR+1 overflows/underflows). PSC is always
// preloaded, ARR when CR1.ARPE, CCRx when CCMRx.OCxPE, RCR always; all transfer at UEV/UG.
// CR1: CEN, UDIS (no UEV), URS (UG sets no UIF), OPM (CEN clears at UEV), DIR (read-only
// and reporting the live direction while CMS != 0), CMS, ARPE, CKD (dead-time clock),
// UIFREMAP (UIF copied to CNT bit 31). ARR = 0 blocks the counter. EGR: UG (reinitialise
// counter + prescaler, transfer preloads, UIF unless URS), CCxG/TG/COMG/BG/B2G set flags.
// SR is rc_w0. Compare: CCxIF sets when CNT reaches CCRx (output mode); in center-aligned
// mode only when counting down (CMS=1), up (CMS=2) or both (CMS=3).
// Model contract: the next instant a flag can change (UIF clear or a preload/OPM/AOE
// pending; CCxIF clear) is one scheduled Clock event; nothing is scheduled while every such
// flag is already set, the counter is stopped or blocked, so there is no event storm.
//
// Not modelled -> strict abort (std::runtime_error naming the timer and the feature): slave
// mode / encoder (SMCR.SMS != 0), external clock mode 2 (SMCR.ECE), input capture
// (CCxS != 0), OCREF clear (OCxCE), OCxM modes 8-15, DMA requests (DIER bits 8-14) and DMA
// bursts (DMAR), commutation preload (CR2.CCPC), break inputs (BDTR.BKE/BK2E), channels 5/6,
// option-register remaps. TRGO (CR2.MMS/MMS2), TS/ETF etc. are stored: they only matter to
// a consumer (ADC/DAC trigger), which aborts itself if it selects a hardware trigger.
//
// call("state") -> {"running", "cnt", "arr", "psc", "clk_hz", "update_hz", "counter"
//   ("up"|"down"|"center1..3"), "channels": [{"ch", "enabled", "mode" ("frozen" |
//   "active_on_match" | "inactive_on_match" | "toggle" | "force_inactive" | "force_active" |
//   "pwm1" | "pwm2"), "polarity" ("high"|"low"), "ccr", "ref_duty", "duty"} +
//   on complementary channels {"enabled_n", "polarity_n", "duty_n", "dead_time_ns"}]}
//   ref_duty: fraction of the period OCxREF is active (PWM1 up: CCR/(ARR+1), down:
//   (CCR+1)/(ARR+1), center: CCR/ARR; PWM2 = 1 - PWM1; toggle 0.5; forced 0/1; null for
//   frozen / active / inactive on match (a static level). duty / duty_n: active fraction of
//   OCx / OCxN; with both outputs enabled each loses the dead time once per period.
//   enabled needs CCxE (and BDTR.MOE on timers with BDTR).
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "stest/clock.hpp"
#include "stest/model_context.hpp"
#include "stest/peripheral.hpp"

namespace stest::stm32l4 {

class Tim : public Peripheral {
 public:
  Tim(std::string name, const nlohmann::json& params, ModelContext& ctx);
  std::string_view name() const override { return name_; }
  uint32_t read(uint32_t off, unsigned width) override;
  void write(uint32_t off, unsigned width, uint32_t v) override;
  void reset() override;
  void connect(ModelContext& ctx) override;
  nlohmann::json call(std::string_view op, const nlohmann::json& args) override;

 private:
  enum class Kind { Basic, General, Advanced };
  enum class Mode { Up, Down, Center };
  enum Src { kUpdate, kCc, kBreak, kTrgCom, kNumSrc };
  using u128 = unsigned __int128;
  static constexpr int64_t kNever = INT64_MAX;

  // --- configuration helpers ---
  [[noreturn]] void not_modelled(const std::string& what, uint32_t off, uint32_t v) const;
  void check_write(uint32_t off, uint32_t v) const;
  Mode mode_of(uint32_t cr1) const;
  uint64_t timer_clk() const;
  bool has_bdtr() const { return ncomp_ > 0; }
  uint32_t rep_reload() const { return has_bdtr() ? rcr_a_ : 0; }
  void load_preloads();
  bool preload_pending() const;
  bool aoe_pending() const;
  void apply_aoe();
  uint32_t ccr_reg(unsigned ch) const { return regs_.get(0x34 + 4 * ch); }
  bool ocpe(unsigned ch) const;

  // --- counter position (valid while moving()) ---
  bool moving() const { return running_ && arr_a_ != 0 && clk_ != 0; }
  int64_t period() const;  // L: ARR+1 (up/down) or 2*ARR (center)
  u128 unit() const { return static_cast<u128>(psc_a_ + 1u) * 1000000000u; }
  int64_t q_at(uint64_t t_ns) const;
  uint64_t time_of(int64_t q) const;
  uint32_t cnt_of(int64_t q) const;
  bool down_of(int64_t q) const;
  int64_t q_from(uint32_t cnt, bool down) const;
  int64_t wraps_upto(int64_t q) const;
  int64_t wrap_point(int64_t n) const;
  bool is_wrap(int64_t q) const;
  uint32_t rep_at(int64_t q) const;
  int64_t next_uev(int64_t qf) const;
  int64_t next_match(unsigned ch, int64_t qf) const;
  void move_anchor(int64_t q);
  void freeze();              // running -> held cnt_/down_/rep_ (anchor kept at the last tick)
  void thaw(bool restart);    // held -> running; restart: prescaler starts now

  // --- events / flags ---
  void do_ug();
  void on_event();
  void schedule();
  void schedule_from(int64_t qf);
  void cancel_event();
  void set_sr(uint32_t bits);
  void update_irqs();

  // --- state report ---
  std::optional<double> ref_duty(unsigned ch, unsigned ocm) const;
  double dead_time_ns(uint64_t clk) const;

  std::string name_;
  ModelContext& ctx_;
  Clock& clock_;
  const ClockTree* clocks_ = nullptr;
  Kind kind_;
  unsigned width_;
  uint32_t max_;
  unsigned nch_, ncomp_;
  bool up_only_, apb2_, has_smcr_, has_ch56_;
  std::array<int, kNumSrc> irq_{-1, -1, -1, -1};
  std::array<char, kNumSrc> src_tag_{};  // addresses identify our sources on shared IRQs
  std::array<bool, kNumSrc> level_{};
  std::vector<std::pair<uint32_t, uint32_t>> opt_;
  uint32_t cr1_m_, cr2_m_, dier_m_, sr_m_, egr_m_, ccer_m_ = 0, bdtr_m_ = 0, rcr_m_ = 0;
  RegisterFile regs_;

  // Active (shadow) registers; regs_ holds the preload values.
  uint32_t psc_a_ = 0, arr_a_ = 0, rcr_a_ = 0;
  std::array<uint32_t, 4> ccr_a_{};
  // Running counter: tick q0_ happened exactly at anchor_ / clk_ ns.
  bool running_ = false;
  Mode mode_ = Mode::Up;
  unsigned cms_ = 0;
  uint64_t clk_ = 0;
  u128 anchor_ = 0;
  int64_t q0_ = 0;
  uint32_t r0_ = 0;  // repetition: wraps still to skip before the next UEV, at q0_
  // Held counter (authoritative while !moving()).
  uint32_t cnt_ = 0, rep_ = 0;
  bool down_ = false;
  // Pending Clock event (for position ev_q_).
  EventId ev_ = 0;
  bool has_ev_ = false;
  int64_t ev_q_ = 0;
};

}  // namespace stest::stm32l4
