// Runtime implementation of the __stest_mmio_* ABI. This TU is NOT compiled
// with the MMIO pass (its own volatile accesses must not recurse).
#include "stest/mmio_windows.hpp"
#include "stest/mmio.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include "stest/runtime.hpp"
#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace {
stest::Clock g_clock;
stest::Bus g_bus;
uint64_t g_cost_ns = 25;
stest::DeliveryHook g_hook = nullptr;
uint64_t g_quantum_end = UINT64_MAX;
stest::QuantumHook g_qhook = nullptr;
stest::AbortHook g_abort_hook = nullptr;

// ---- poll streak (see runtime.hpp) ----
constexpr unsigned kMaxTuples = 8;
struct Tuple { uint64_t addr; uint64_t value; unsigned width; };
uint32_t g_skip_threshold = 64;
uint32_t g_streak = 0;         // loads in the current streak
unsigned g_ntuples = 0;
Tuple g_tuples[kMaxTuples];
uint64_t g_break_epoch = 0;    // bumped by poll_streak_break()
bool g_inited = false;

using stest::kCoreBase;
using stest::kCoreSize;
using stest::kPeriphBase;
using stest::kPeriphSize;

inline bool in_window(uint64_t a) { return stest::in_mmio_window(a); }

// Clock event callbacks (e.g. SysTick wraps) fire here; nothing may escape into
// instrumented C.
void run_due_events() {
  try { g_clock.run_due(); }
  catch (const stest::NotModelled& e) { stest_rt_abort("%s", e.what()); }
  catch (const std::exception& e) { stest_rt_abort("exception in clock event: %s", e.what()); }
  catch (...) { stest_rt_abort("unknown exception in clock event"); }
}
// After the clock moved: fire due events, then yield at the quantum end.
inline void settle() {
  if (__builtin_expect(g_clock.next_event_ns() <= g_clock.now_ns(), 0)) run_due_events();
  // Quantum end: events due at <= now have fired; hand control back (blocks).
  if (__builtin_expect(g_clock.now_ns() >= g_quantum_end, 0) && g_qhook) g_qhook();
}
inline void charge() {
  g_clock.advance(g_cost_ns);
  settle();
}
void deliver() {
  if (!g_hook) return;
  try { g_hook(); }
  catch (const stest::NotModelled& e) { stest_rt_abort("%s", e.what()); }
  catch (const std::exception& e) { stest_rt_abort("exception in delivery hook: %s", e.what()); }
  catch (...) { stest_rt_abort("unknown exception in delivery hook"); }
}

inline void streak_reset() {
  g_streak = 0;
  g_ntuples = 0;
}

// Record a load; returns true when the streak reached the skip threshold.
inline bool streak_load(uint64_t a, unsigned width, uint64_t v) {
  for (unsigned i = 0; i < g_ntuples; ++i) {
    Tuple& t = g_tuples[i];
    if (t.addr == a && t.width == width) {
      if (t.value != v) break;  // value changed: new streak seeded below
      return ++g_streak >= g_skip_threshold;
    }
    if (i + 1 == g_ntuples && g_ntuples < kMaxTuples) {  // new tuple, room left
      g_tuples[g_ntuples++] = {a, v, width};
      return ++g_streak >= g_skip_threshold;
    }
  }
  g_tuples[0] = {a, v, width};  // first load of a new streak
  g_ntuples = 1;
  g_streak = 1;
  return g_streak >= g_skip_threshold;
}

void deliver();
// Jump to where spinning would next see a change, as a charge reaching it would.
void fast_forward() {
  streak_reset();
  uint64_t target = std::min(g_clock.next_event_ns(), g_quantum_end);
  uint64_t now = g_clock.now_ns();
  if (target == UINT64_MAX || target <= now) return;
  g_clock.advance(target - now);
  settle();
  deliver();
}

[[noreturn]] void abort_access(const stest::UnmodelledAccess& e) {
  if (e.read_only)
    stest_rt_abort("write to read-only register 0x%08x (write%u, %s)", (unsigned)e.addr, e.width * 8,
                   e.peripheral.c_str());
  if (e.peripheral.empty())
    stest_rt_abort("unmodelled %s%u 0x%08x", e.is_write ? "write" : "read", e.width * 8,
                   (unsigned)e.addr);
  stest_rt_abort("unmodelled %s%u 0x%08x (%s)", e.is_write ? "write" : "read", e.width * 8,
                 (unsigned)e.addr, e.peripheral.c_str());
}

// "stest: <PERIPH>: <feature> not modelled (<read|write><bits> 0x<addr>[ = 0x<value>])"
[[noreturn]] void abort_not_modelled(const stest::NotModelled& e, uint64_t a, unsigned bytes, bool is_write,
                                     const uint32_t* v) {
  if (v)
    stest_rt_abort("%s (%s%u 0x%08x = 0x%08x)", e.what(), is_write ? "write" : "read", bytes * 8, (unsigned)a,
                   (unsigned)*v);
  stest_rt_abort("%s (%s%u 0x%08x)", e.what(), is_write ? "write" : "read", bytes * 8, (unsigned)a);
}

uint32_t bus_read(uint64_t a, unsigned bytes) {
  try { return g_bus.read(a, bytes); }
  catch (const stest::UnmodelledAccess& e) { abort_access(e); }
  catch (const stest::NotModelled& e) { abort_not_modelled(e, a, bytes, false, nullptr); }
  catch (const stest::ClockDisabled& e) { stest_rt_abort("%s", e.what()); }
  catch (const std::exception& e) { stest_rt_abort("exception in model at 0x%08x: %s", (unsigned)a, e.what()); }
  catch (...) { stest_rt_abort("unknown exception in model at 0x%08x", (unsigned)a); }
}
void bus_write(uint64_t a, unsigned bytes, uint32_t v) {
  try { g_bus.write(a, bytes, v); }
  catch (const stest::UnmodelledAccess& e) { abort_access(e); }
  catch (const stest::NotModelled& e) { abort_not_modelled(e, a, bytes, true, &v); }
  catch (const stest::ClockDisabled& e) { stest_rt_abort("%s", e.what()); }
  catch (const std::exception& e) { stest_rt_abort("exception in model at 0x%08x: %s", (unsigned)a, e.what()); }
  catch (...) { stest_rt_abort("unknown exception in model at 0x%08x", (unsigned)a); }
}

inline uint64_t do_load(const volatile void* p, unsigned bytes) {
  uint64_t a = (uint64_t)(uintptr_t)p;
  charge();
  uint64_t r;
  if (__builtin_expect(!in_window(a), 1)) {
    switch (bytes) {
      case 1: r = *(const volatile uint8_t*)p; break;
      case 2: r = *(const volatile uint16_t*)p; break;
      case 4: r = *(const volatile uint32_t*)p; break;
      default: r = *(const volatile uint64_t*)p; break;
    }
  } else if (bytes == 8) {  // window peripherals are 32-bit: low word, then high
    uint64_t lo = bus_read(a, 4);
    uint64_t hi = bus_read(a + 4, 4);
    r = lo | (hi << 32);
  } else {
    r = bus_read(a, bytes);
  }
  uint64_t epoch = g_break_epoch;
  deliver();
  if (__builtin_expect(g_skip_threshold != 0, 1)) {
    if (epoch != g_break_epoch) streak_reset();  // exception taken: this load doesn't count
    else if (__builtin_expect(streak_load(a, bytes, r), 0)) fast_forward();
  }
  return r;
}

inline void do_store(volatile void* p, unsigned bytes, uint64_t v) {
  uint64_t a = (uint64_t)(uintptr_t)p;
  streak_reset();
  charge();
  if (__builtin_expect(!in_window(a), 1)) {
    switch (bytes) {
      case 1: *(volatile uint8_t*)p = (uint8_t)v; break;
      case 2: *(volatile uint16_t*)p = (uint16_t)v; break;
      case 4: *(volatile uint32_t*)p = (uint32_t)v; break;
      default: *(volatile uint64_t*)p = v; break;
    }
  } else if (bytes == 8) {
    bus_write(a, 4, (uint32_t)v);
    bus_write(a + 4, 4, (uint32_t)(v >> 32));
  } else {
    bus_write(a, bytes, (uint32_t)v);
  }
  deliver();
}

// Widest of 4/2/1 bytes that fits in `left` and is naturally aligned at `a` (and `b`).
inline unsigned chunk_width(uint64_t a, uint64_t b, size_t left) {
  for (unsigned w = 4; w > 1; w >>= 1)
    if (left >= w && ((a | b) & (w - 1)) == 0) return w;
  return 1;
}
}  // namespace

namespace stest {
Clock& runtime_clock() { return g_clock; }
Bus& runtime_bus() { return g_bus; }
uint64_t access_cost_ns() { return g_cost_ns; }
void set_access_cost_ns(uint64_t ns) { g_cost_ns = ns; }
void set_delivery_hook(DeliveryHook h) { g_hook = h; }
void set_quantum_end(uint64_t t_ns) { g_quantum_end = t_ns; }
uint64_t quantum_end() { return g_quantum_end; }
void set_quantum_hook(QuantumHook h) { g_qhook = h; }
void set_abort_hook(AbortHook h) { g_abort_hook = h; }
void set_poll_skip_threshold(uint32_t loads) {
  g_skip_threshold = loads;
  streak_reset();
}
uint32_t poll_skip_threshold() { return g_skip_threshold; }
void poll_streak_break() {
  ++g_break_epoch;
  streak_reset();
}

uint32_t shim_access(uint64_t addr, unsigned width, bool is_write, ShimOp op, void* ctx) {
  if (is_write) streak_reset();
  charge();
  uint32_t r = 0;
  try {
    Peripheral* p = g_bus.require_clock(addr, width, is_write);
    try {
      r = op(*p, ctx);
    } catch (const ReadOnlyRegister&) {
      throw UnmodelledAccess(addr, width, true, std::string(p->name()), /*ro=*/true);
    } catch (const UnmodelledRegister&) {
      throw UnmodelledAccess(addr, width, is_write, std::string(p->name()));
    }
  }
  catch (const UnmodelledAccess& e) { abort_access(e); }
  catch (const NotModelled& e) { abort_not_modelled(e, addr, width, is_write, nullptr); }  // shim: value not known
  catch (const ClockDisabled& e) { stest_rt_abort("%s", e.what()); }
  catch (const std::exception& e) { stest_rt_abort("exception in model at 0x%08x: %s", (unsigned)addr, e.what()); }
  catch (...) { stest_rt_abort("unknown exception in model at 0x%08x", (unsigned)addr); }
  if (is_write) {
    deliver();
    return 0;
  }
  uint64_t epoch = g_break_epoch;
  deliver();
  if (__builtin_expect(g_skip_threshold != 0, 1)) {
    if (epoch != g_break_epoch) streak_reset();
    else if (streak_load(addr, width, r)) fast_forward();
  }
  return r;
}
}  // namespace stest

extern "C" {

void stest_rt_abort(const char* fmt, ...) {
  // Formatted into a fixed buffer (no allocation: may run on any thread, in any state).
  static char buf[1024];
  int n = snprintf(buf, sizeof buf, "stest: ");
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
  va_end(ap);
  fputs(buf, stderr);
  fputc('\n', stderr);
  fflush(stderr);
  fflush(stdout);
  if (stest::AbortHook h = g_abort_hook) {
    g_abort_hook = nullptr;  // no recursion if the hook itself aborts
    h(buf);
  }
  _exit(70);
}

uint64_t stest_rt_now_ns(void) { return g_clock.now_ns(); }

void stest_rt_init(void) {
  if (g_inited) return;
  g_inited = true;
#if defined(__linux__)
  // Reserve both windows so no host object can live there.
  struct { uint64_t base, size; } w[2] = {{kPeriphBase, kPeriphSize}, {kCoreBase, kCoreSize}};
  for (auto& r : w) {
#ifdef MAP_FIXED_NOREPLACE
    void* p = mmap((void*)r.base, r.size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
#else
    void* p = mmap((void*)r.base, r.size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);  // hint only
#endif
    if (p == MAP_FAILED || (uint64_t)(uintptr_t)p != r.base)
      stest_rt_abort("cannot reserve address window 0x%08x+0x%x", (unsigned)r.base, (unsigned)r.size);
  }
#elif defined(__APPLE__)
  // __PAGEZERO covers the low 4 GiB; verify the image really sits above it.
  if ((uint64_t)(uintptr_t)&stest_rt_init < (1ull << 32))
    stest_rt_abort("host image mapped below 4 GiB; peripheral windows not protected");
#endif
}

uint8_t __stest_mmio_load8(const volatile void* a) { return (uint8_t)do_load(a, 1); }
uint16_t __stest_mmio_load16(const volatile void* a) { return (uint16_t)do_load(a, 2); }
uint32_t __stest_mmio_load32(const volatile void* a) { return (uint32_t)do_load(a, 4); }
uint64_t __stest_mmio_load64(const volatile void* a) { return do_load(a, 8); }
void __stest_mmio_store8(volatile void* a, uint8_t v) { do_store(a, 1, v); }
void __stest_mmio_store16(volatile void* a, uint16_t v) { do_store(a, 2, v); }
void __stest_mmio_store32(volatile void* a, uint32_t v) { do_store(a, 4, v); }
void __stest_mmio_store64(volatile void* a, uint64_t v) { do_store(a, 8, v); }

void __stest_mmio_memcpy(volatile void* dst, const volatile void* src, size_t n) {
  uint64_t d = (uint64_t)(uintptr_t)dst, s = (uint64_t)(uintptr_t)src;
  // No d == s shortcut: a volatile self-copy still performs its reads and writes.
  if (d > s && d - s < n) {  // dst overlaps the tail of src: copy backwards
    for (size_t e = n; e > 0;) {
      unsigned w = 1;
      for (unsigned c = 4; c > 1; c >>= 1)
        if (e >= c && (((d + e - c) | (s + e - c)) & (c - 1)) == 0) { w = c; break; }
      e -= w;
      do_store((volatile char*)dst + e, w, do_load((const volatile char*)src + e, w));
    }
    return;
  }
  for (size_t o = 0; o < n;) {
    unsigned w = chunk_width(d + o, s + o, n - o);
    do_store((volatile char*)dst + o, w, do_load((const volatile char*)src + o, w));
    o += w;
  }
}

void __stest_mmio_memset(volatile void* dst, int c, size_t n) {
  uint64_t d = (uint64_t)(uintptr_t)dst;
  uint32_t b = (uint8_t)c;
  for (size_t o = 0; o < n;) {
    unsigned w = chunk_width(d + o, 0, n - o);
    do_store((volatile char*)dst + o, w, b * (w == 4 ? 0x01010101u : w == 2 ? 0x0101u : 1u));
    o += w;
  }
}

}  // extern "C"
