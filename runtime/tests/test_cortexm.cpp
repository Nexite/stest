#include <doctest/doctest.h>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "stest/cortexm.hpp"
#include "stest/cortexm_c.h"
#include "stest/mmio.h"
#include "stest/runtime.hpp"
using namespace stest;

namespace {
constexpr size_t kExc = 16 + 64;
constexpr uint64_t ISER = 0xE000E100, ICER = 0xE000E180, ISPR = 0xE000E200, ICPR = 0xE000E280,
                   IABR = 0xE000E300, IPR = 0xE000E400;
constexpr uint64_t CPUID = 0xE000ED00, ICSR = 0xE000ED04, VTOR = 0xE000ED08, AIRCR = 0xE000ED0C,
                   SHPR3 = 0xE000ED20, CPACR = 0xE000ED88;
constexpr uint64_t ST_CSR = 0xE000E010, ST_RVR = 0xE000E014, ST_CVR = 0xE000E018, ST_CALIB = 0xE000E01C;
constexpr uint64_t MS = 1'000'000;

std::vector<std::string> g_log;
int g_count[kExc];
Cortexm* g_core = nullptr;
uint32_t g_sysclk = 80'000'000;
uint32_t sysclk() { return g_sysclk; }

template <size_t N> void count_h() {
  ++g_count[N];
  g_log.push_back("exc" + std::to_string(N));
}
template <size_t... I> void fill(IrqHandler* t, std::index_sequence<I...>) { ((t[I] = &count_h<I>), ...); }

struct Rig {
  Bus bus;
  Clock clock;
  IrqHandler table[kExc];
  std::unique_ptr<Cortexm> core;
  explicit Rig(unsigned prio_bits = 4) {
    fill(table, std::make_index_sequence<kExc>{});
    g_log.clear();
    for (int& c : g_count) c = 0;
    g_sysclk = 80'000'000;
    core = std::make_unique<Cortexm>(bus, clock, prio_bits, table, kExc);
    g_core = core.get();
  }
  ~Rig() { g_core = nullptr; }
  void w32(uint64_t a, uint32_t v) { bus.write(a, 4, v); }
  uint32_t r32(uint64_t a) { return bus.read(a, 4); }
  void enable(int irq) { w32(ISER + 4 * (irq / 32), 1u << (irq % 32)); }
  void prio(int irq, uint8_t p) { bus.write(IPR + irq, 1, p); }
  int count_irq(int irqn) const { return g_count[irqn + 16]; }
  void step_to(uint64_t t, uint64_t step) {  // advance in steps, firing events + delivering
    while (clock.now_ns() < t) {
      clock.advance(std::min(step, t - clock.now_ns()));
      clock.run_due();
      core->deliver();
    }
  }
};

void low_h() {  // IRQ 3: pends IRQ 4 and hits a delivery point
  g_log.push_back("low_enter");
  g_core->set_pending(4);
  g_core->deliver();
  g_log.push_back("low_exit");
}
void high_h() { g_log.push_back("high"); }
}  // namespace

// ---- brief (a)-(e) ----

TEST_CASE("cortexm (a): PRIMASK holds an enabled pending IRQ until cleared") {
  Rig r;
  r.enable(5);
  r.core->set_primask(true);
  r.core->set_pending(5);
  r.core->deliver();
  CHECK(r.count_irq(5) == 0);
  CHECK(r.core->pending(5));
  r.core->set_primask(false);
  CHECK(r.count_irq(5) == 1);
  CHECK_FALSE(r.core->pending(5));
}

TEST_CASE("cortexm (b): higher-priority IRQ pended in a lower-priority handler preempts") {
  Rig r;
  r.table[16 + 3] = low_h;
  r.table[16 + 4] = high_h;
  r.enable(3); r.enable(4);
  r.prio(3, 0x80); r.prio(4, 0x40);
  r.core->set_pending(3);
  r.core->deliver();
  CHECK(g_log == std::vector<std::string>{"low_enter", "high", "low_exit"});
}

TEST_CASE("cortexm (c): SysTick at 80 MHz, LOAD=79999 fires exactly 10 times in 10 ms") {
  SUBCASE("fine steps") {
    Rig r;
    r.core->set_core_clock_getter(sysclk);
    r.w32(ST_RVR, 79999);
    r.w32(ST_CVR, 0);
    r.w32(ST_CSR, 0x3);  // ENABLE | TICKINT
    r.step_to(10 * MS, 10'000);
    CHECK(r.count_irq(-1) == 10);
  }
  SUBCASE("wfi loop") {
    Rig r;
    r.core->set_core_clock_getter(sysclk);
    r.w32(ST_RVR, 79999);
    r.w32(ST_CSR, 0x3);
    r.core->set_quantum_end(10 * MS);
    while (r.clock.now_ns() < 10 * MS) r.core->wfi();
    CHECK(r.count_irq(-1) == 10);
    CHECK(r.clock.now_ns() == 10 * MS);
  }
}

TEST_CASE("cortexm (d): disabled IRQ pended stays in ISPR and fires on later enable") {
  Rig r;
  r.core->set_pending(7);
  r.core->deliver();
  CHECK(r.count_irq(7) == 0);
  CHECK(r.r32(ISPR) == (1u << 7));
  r.enable(7);
  CHECK(r.r32(ISER) == (1u << 7));
  r.core->deliver();
  CHECK(r.count_irq(7) == 1);
  CHECK(r.r32(ISPR) == 0);
}

TEST_CASE("cortexm (e): wfi advances to the next event before the quantum end") {
  Rig r;
  bool fired = false;
  r.clock.schedule_at(3 * MS, [&] { fired = true; });
  r.core->set_quantum_end(5 * MS);
  r.core->wfi();
  CHECK(r.clock.now_ns() == 3 * MS);
  CHECK(fired);
  r.core->wfi();  // no more events: stop at the quantum end
  CHECK(r.clock.now_ns() == 5 * MS);
  r.core->wfi();  // already at the quantum end: no progress, no crash
  CHECK(r.clock.now_ns() == 5 * MS);
}

int g_qhook_calls = 0;
TEST_CASE("cortexm: wfi calls the quantum hook once it reaches the quantum end") {
  Rig r;
  g_qhook_calls = 0;
  r.core->set_quantum_hook([] { ++g_qhook_calls; });
  r.clock.schedule_at(3 * MS, [] {});
  r.core->set_quantum_end(5 * MS);
  r.core->wfi();  // stops at the event: no hook
  CHECK(g_qhook_calls == 0);
  r.core->wfi();  // reaches the quantum end: hook
  CHECK(r.clock.now_ns() == 5 * MS);
  CHECK(g_qhook_calls == 1);
  r.core->wfi();  // still at the quantum end: hook again (the node would have moved it)
  CHECK(g_qhook_calls == 2);
}

// ---- dispatcher ----

TEST_CASE("cortexm: equal priority does not preempt; it tail-chains") {
  Rig r;
  r.table[16 + 3] = low_h;
  r.table[16 + 4] = high_h;
  r.enable(3); r.enable(4);
  r.prio(3, 0x40); r.prio(4, 0x40);
  r.core->set_pending(3);
  r.core->deliver();
  CHECK(g_log == std::vector<std::string>{"low_enter", "low_exit", "high"});
}

TEST_CASE("cortexm: PRIGROUP splits group/subpriority; only group priority preempts") {
  Rig r;
  r.table[16 + 3] = low_h;
  r.table[16 + 4] = high_h;
  r.enable(3); r.enable(4);
  r.prio(3, 0x30); r.prio(4, 0x10);
  SUBCASE("PRIGROUP=5: bits 7:6 group -> same group, no preemption") {
    r.w32(AIRCR, 0x05FA0000u | (5u << 8));
    CHECK((r.r32(AIRCR) & 0x700) == 0x500);
    r.core->set_pending(3);
    r.core->deliver();
    CHECK(g_log == std::vector<std::string>{"low_enter", "low_exit", "high"});
  }
  SUBCASE("PRIGROUP=3: all 4 implemented bits group -> preempts") {
    r.w32(AIRCR, 0x05FA0000u | (3u << 8));
    r.core->set_pending(3);
    r.core->deliver();
    CHECK(g_log == std::vector<std::string>{"low_enter", "high", "low_exit"});
  }
  SUBCASE("AIRCR write without VECTKEY is ignored") {
    r.w32(AIRCR, 5u << 8);
    CHECK((r.r32(AIRCR) & 0x700) == 0);
    CHECK((r.r32(AIRCR) >> 16) == 0xFA05);
  }
}

TEST_CASE("cortexm: simultaneous pendings run by priority, then by exception number") {
  Rig r;
  for (int i : {1, 2, 9}) r.enable(i);
  r.prio(1, 0x80); r.prio(2, 0x20); r.prio(9, 0x20);
  r.core->set_primask(true);
  r.core->set_pending(9); r.core->set_pending(1); r.core->set_pending(2);
  r.core->set_primask(false);
  CHECK(g_log == std::vector<std::string>{"exc18", "exc25", "exc17"});
}

TEST_CASE("cortexm: BASEPRI masks priorities >= its value; BASEPRI_MAX only raises") {
  Rig r;
  r.enable(1); r.enable(2);
  r.prio(1, 0x40); r.prio(2, 0x30);
  r.core->set_basepri(0x40);
  r.core->set_pending(1); r.core->set_pending(2);
  r.core->deliver();
  CHECK(r.count_irq(2) == 1);
  CHECK(r.count_irq(1) == 0);
  r.core->set_basepri_max(0x80);  // lower priority: ignored
  CHECK(r.core->basepri() == 0x40);
  r.core->set_basepri_max(0x20);
  CHECK(r.core->basepri() == 0x20);
  r.core->set_basepri(0);  // delivery point
  CHECK(r.count_irq(1) == 1);
}

TEST_CASE("cortexm: FAULTMASK blocks delivery until cleared") {
  Rig r;
  r.enable(0);
  r.core->set_faultmask(true);
  r.core->set_pending(0);
  r.core->deliver();
  CHECK(r.count_irq(0) == 0);
  r.core->set_faultmask(false);
  CHECK(r.count_irq(0) == 1);
}

namespace {
uint32_t g_seen_iabr, g_seen_icsr, g_seen_ipsr;
Rig* g_rig;
void probe_h() {
  g_seen_iabr = g_rig->r32(IABR);
  g_seen_icsr = g_rig->r32(ICSR);
  g_seen_ipsr = g_core->ipsr();
  g_log.push_back("probe");
}
int g_self_runs = 0;
void self_h() {
  if (++g_self_runs < 3) { g_core->set_pending(6); g_core->deliver(); }
  g_log.push_back("self" + std::to_string(g_self_runs));
}
}  // namespace

TEST_CASE("cortexm: IABR/ICSR/IPSR inside a handler") {
  Rig r;
  g_rig = &r;
  r.table[16 + 6] = probe_h;
  r.enable(6);
  r.core->set_pending(6);
  r.core->deliver();
  CHECK(g_seen_iabr == (1u << 6));
  CHECK((g_seen_icsr & 0x1FF) == 22);  // VECTACTIVE
  CHECK(g_seen_ipsr == 22);
  CHECK(r.r32(IABR) == 0);
  CHECK(r.core->ipsr() == 0);
  CHECK(g_log == std::vector<std::string>{"probe"});
}

TEST_CASE("cortexm: an IRQ re-pending itself never nests") {
  Rig r;
  g_self_runs = 0;
  r.table[16 + 6] = self_h;
  r.enable(6);
  r.core->set_pending(6);
  r.core->deliver();
  CHECK(g_log == std::vector<std::string>{"self1", "self2", "self3"});  // sequential, not nested
}

TEST_CASE("cortexm: NVIC ICER/ICPR, IPR byte/word access, implemented priority bits") {
  Rig r;
  r.enable(3); r.enable(33);
  CHECK(r.r32(ISER) == (1u << 3));
  CHECK(r.r32(ISER + 4) == (1u << 1));
  r.w32(ICER, 1u << 3);
  CHECK(r.r32(ISER) == 0);
  CHECK_FALSE(r.core->enabled(3));
  r.w32(ISPR, 1u << 4);
  CHECK(r.core->pending(4));
  r.w32(ICPR, 1u << 4);
  CHECK_FALSE(r.core->pending(4));
  r.prio(5, 0xFF);
  CHECK(r.bus.read(IPR + 5, 1) == 0xF0);  // 4 implemented bits
  CHECK(r.core->priority(5) == 0xF0);
  r.w32(IPR + 4, 0x10203040);
  CHECK(r.r32(IPR + 4) == 0x10203040);
  CHECK(r.core->priority(6) == 0x20);
  // IRQs beyond the table are RAZ/WI.
  r.w32(ISER + 4 * 7, 0xFFFFFFFF);
  CHECK(r.r32(ISER + 4 * 7) == 0);
  CHECK_THROWS_AS(r.r32(0xE000E120), UnmodelledAccess);  // reserved gap
  CHECK_THROWS_AS(r.r32(0xE000ED40), UnmodelledAccess);  // SCB ID_PFR0 not modelled
}

TEST_CASE("cortexm: set_pending validation") {
  Rig r;
  CHECK_THROWS_AS(r.core->set_pending(64), std::out_of_range);
  CHECK_THROWS_AS(r.core->set_pending(-14), std::out_of_range);  // NMI not supported
  CHECK_THROWS_AS(r.core->set_pending(-17), std::out_of_range);
  CHECK_NOTHROW(r.core->set_pending(Cortexm::kSVCall_IRQn));
}

TEST_CASE("cortexm: SCB ICSR PendSV/SysTick set/clear, VECTPENDING, SHPR, misc") {
  Rig r;
  CHECK(r.r32(CPUID) == 0x410FC241);
  r.w32(ICSR, 1u << 28);  // PENDSVSET
  CHECK(r.core->pending(Cortexm::kPendSV_IRQn));
  CHECK((r.r32(ICSR) & (1u << 28)) != 0);
  CHECK(((r.r32(ICSR) >> 12) & 0x1FF) == 14);  // VECTPENDING
  r.w32(ICSR, 1u << 26);  // PENDSTSET
  CHECK(r.core->pending(Cortexm::kSysTick_IRQn));
  r.w32(ICSR, 1u << 25);  // PENDSTCLR
  CHECK_FALSE(r.core->pending(Cortexm::kSysTick_IRQn));
  r.core->set_pending(2);
  CHECK((r.r32(ICSR) & (1u << 22)) != 0);  // ISRPENDING (external)
  r.core->clear_pending(2);
  r.core->deliver();
  CHECK(r.count_irq(-2) == 1);
  // SysTick priority via SHPR3 byte (CMSIS SCB->SHP[11]).
  r.bus.write(SHPR3 + 3, 1, 0xFF);
  CHECK(r.core->priority(-1) == 0xF0);
  CHECK(r.r32(SHPR3) == 0xF0000000);
  r.w32(VTOR, 0x08000000);
  CHECK(r.r32(VTOR) == 0x08000000);
  r.w32(CPACR, 0xF << 20);
  CHECK(r.r32(CPACR) == (0xFu << 20));
  CHECK_THROWS_AS(r.w32(AIRCR, 0x05FA0004), std::runtime_error);  // SYSRESETREQ
}

// ---- SysTick ----

TEST_CASE("systick: CVR reflects elapsed time; COUNTFLAG set on wrap, cleared by CSR read") {
  Rig r;
  r.core->set_core_clock_getter(sysclk);
  r.w32(ST_RVR, 79999);
  r.w32(ST_CVR, 1234);  // any write clears to 0
  CHECK(r.r32(ST_CVR) == 0);
  r.w32(ST_CSR, 0x5);  // ENABLE | CLKSOURCE, no TICKINT
  CHECK(r.r32(ST_CVR) == 0);
  r.clock.advance(13);  // one 12.5 ns tick -> reload
  CHECK(r.r32(ST_CVR) == 79999);
  r.clock.advance(500'000 - 13);
  CHECK(r.r32(ST_CVR) == 40000);
  CHECK((r.r32(ST_CSR) & (1u << 16)) == 0);
  r.clock.advance(500'000);  // exactly 1 ms: transition to 0
  r.clock.run_due();
  CHECK(r.r32(ST_CVR) == 0);
  uint32_t csr = r.r32(ST_CSR);
  CHECK((csr & (1u << 16)) != 0);
  CHECK((csr & 0x7) == 0x5);
  CHECK((r.r32(ST_CSR) & (1u << 16)) == 0);  // cleared by the read
  CHECK(r.count_irq(-1) == 0);                // TICKINT=0
  CHECK_FALSE(r.core->pending(-1));
  CHECK(r.r32(ST_CALIB) == 0xC0000000);
}

TEST_CASE("systick: COUNTFLAG catches up even without run_due; CVR write clears it") {
  Rig r;
  r.core->set_core_clock_getter(sysclk);
  r.w32(ST_RVR, 79999);
  r.w32(ST_CSR, 0x5);
  r.clock.advance(1 * MS + 100);
  r.w32(ST_CVR, 0);
  CHECK((r.r32(ST_CSR) & (1u << 16)) == 0);
  r.clock.advance(1 * MS - 1);
  CHECK((r.r32(ST_CSR) & (1u << 16)) == 0);  // restarted at the CVR write
  r.clock.advance(1);
  CHECK((r.r32(ST_CSR) & (1u << 16)) != 0);
}

TEST_CASE("systick: LOAD change applies from the next reload; disable freezes CVR") {
  Rig r;
  r.core->set_core_clock_getter(sysclk);
  r.w32(ST_RVR, 79999);
  r.w32(ST_CSR, 0x7);
  r.step_to(MS / 4, 1000);
  r.w32(ST_RVR, 39999);
  r.step_to(MS - 1000, 1000);
  CHECK(r.count_irq(-1) == 0);
  r.step_to(MS, 1000);
  CHECK(r.count_irq(-1) == 1);
  r.step_to(MS + MS / 2, 1000);
  CHECK(r.count_irq(-1) == 2);  // 0.5 ms period now
  r.step_to(MS + MS / 2 + MS / 4, 1000);
  r.w32(ST_CSR, 0x6);  // disable
  uint32_t v = r.r32(ST_CVR);
  CHECK(v == 20000);
  r.step_to(10 * MS, 100'000);
  CHECK(r.r32(ST_CVR) == v);
  CHECK(r.count_irq(-1) == 2);
  CHECK(r.clock.next_event_ns() == UINT64_MAX);
}

TEST_CASE("systick: core clock changes and the CLKSOURCE=0 external divider") {
  SUBCASE("8 MHz core clock -> 10 ms period") {
    Rig r;
    r.core->set_core_clock_getter(sysclk);
    g_sysclk = 8'000'000;
    r.w32(ST_RVR, 79999);
    r.w32(ST_CSR, 0x7);
    r.step_to(10 * MS - 1000, 1000);
    CHECK(r.count_irq(-1) == 0);
    r.step_to(10 * MS, 1000);
    CHECK(r.count_irq(-1) == 1);
  }
  SUBCASE("CLKSOURCE=0 with divider 8 -> 8 ms period") {
    Rig r;
    r.core->set_core_clock_getter(sysclk);
    r.core->set_systick_external_divider(8);
    CHECK(r.r32(ST_CALIB) == 0x40000000);  // external reference present: NOREF=0
    r.w32(ST_RVR, 79999);
    r.w32(ST_CSR, 0x3);
    r.step_to(8 * MS - 1000, 1000);
    CHECK(r.count_irq(-1) == 0);
    r.step_to(8 * MS, 1000);
    CHECK(r.count_irq(-1) == 1);
  }
  SUBCASE("enabling without a core clock getter throws") {
    Rig r;
    r.w32(ST_RVR, 79999);
    CHECK_THROWS_AS(r.w32(ST_CSR, 0x7), std::runtime_error);
  }
}

TEST_CASE("cortexm: wfi delivers an already-pending IRQ without advancing time") {
  Rig r;
  r.enable(2);
  r.core->set_primask(true);
  r.core->set_pending(2);
  r.core->set_primask(false);  // delivered here
  CHECK(r.count_irq(2) == 1);
  r.core->set_basepri(0x10);
  r.prio(2, 0x00);
  r.core->set_pending(2);       // prio 0 < BASEPRI 0x10: eligible
  r.core->set_quantum_end(5 * MS);
  r.core->wfi();
  CHECK(r.count_irq(2) == 2);
  CHECK(r.clock.now_ns() == 0);
  Rig r2;
  CHECK_THROWS_AS(r2.core->wfi(), std::runtime_error);  // nothing could ever wake it
}

// ---- runtime integration: mmio delivery hook + C API ----

TEST_CASE("cortexm: installed core is driven by mmio accesses and the C API") {
  stest_rt_init();
  static IrqHandler table[kExc];
  fill(table, std::make_index_sequence<kExc>{});
  for (int& c : g_count) c = 0;
  g_log.clear();
  {
    Cortexm core(runtime_bus(), runtime_clock(), 4, table, kExc);
    g_core = &core;
    core.install();
    CHECK(Cortexm::installed() == &core);
    core.set_core_clock_getter(sysclk);
    g_sysclk = 80'000'000;

    stest_rt_disable_irq();
    CHECK(stest_rt_get_primask() == 1);
    __stest_mmio_store32((void*)ISPR, 1u << 9);
    __stest_mmio_store32((void*)ISER, 1u << 9);  // hook runs, but PRIMASK holds it
    CHECK(g_count[16 + 9] == 0);
    stest_rt_enable_irq();
    CHECK(g_count[16 + 9] == 1);

    __stest_mmio_store32((void*)ISPR, 1u << 9);  // enabled: the hook after the store delivers
    CHECK(g_count[16 + 9] == 2);

    stest_rt_set_basepri(0x40);
    CHECK(stest_rt_get_basepri() == 0x40);
    stest_rt_set_basepri_max(0x80);
    CHECK(stest_rt_get_basepri() == 0x40);
    stest_rt_set_basepri(0);
    stest_rt_set_faultmask(1);
    CHECK(stest_rt_get_faultmask() == 1);
    stest_rt_set_faultmask(0);
    CHECK(stest_rt_get_ipsr() == 0);

    uint64_t t0 = runtime_clock().now_ns();
    __stest_mmio_store32((void*)ST_RVR, 79999);
    __stest_mmio_store32((void*)ST_CVR, 0);
    __stest_mmio_store32((void*)ST_CSR, 0x7);
    core.set_quantum_end(UINT64_MAX);
    while (g_count[15] < 3) stest_rt_wfi();
    // Wraps are 1 ms apart from the CSR write (t0 + 3 stores x 25 ns).
    CHECK(runtime_clock().now_ns() - t0 == 3 * MS + 3 * access_cost_ns());
    // Busy-wait on a host variable: each access charges time and the clock event
    // fires from the access path, then the hook delivers SysTick.
    volatile uint32_t dummy = 0;
    while (g_count[15] < 4) (void)__stest_mmio_load32(&dummy);
    CHECK(g_count[15] == 4);
    __stest_mmio_store32((void*)ST_CSR, 0);
  }
  CHECK(Cortexm::installed() == nullptr);
  g_core = nullptr;
  // Blocks were unmapped: a fresh core can map again.
  Cortexm again(runtime_bus(), runtime_clock(), 4, table, kExc);
}

TEST_CASE("cortexm: wfi with PRIMASK set wakes on a pending IRQ without running it") {
  Rig r;
  r.enable(3);
  r.core->set_primask(true);
  r.core->set_pending(3);
  r.core->set_quantum_end(5 * MS);
  r.core->wfi();  // __disable_irq(); __WFI(); idiom: returns, handler held
  CHECK(r.clock.now_ns() == 0);
  CHECK(r.count_irq(3) == 0);
  r.core->set_primask(false);
  CHECK(r.count_irq(3) == 1);
  // Masked by BASEPRI (not PRIMASK): wfi does not wake, it sleeps to the quantum end.
  r.prio(3, 0x80);
  r.core->set_basepri(0x40);
  r.core->set_pending(3);
  r.core->wfi();
  CHECK(r.clock.now_ns() == 5 * MS);
  CHECK(r.count_irq(3) == 1);
}
