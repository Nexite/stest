/* Bench-local model of the intended __stest_mmio_* fast path. Separate TU +
 * noinline so the compiler cannot fold it into the benchmark loop.
 *   out-of-window : direct host access + time charge
 *   in-window     : range check -> 64-entry page table -> model call through a
 *                   function pointer (trivial array-backed register store) + time charge */
#include <stdint.h>
#include <string.h>

#define NOINL __attribute__((noinline))
#define ACCESS_NS 25u

uint64_t stest_now_ns;

typedef uint64_t (*model_read_fn)(void *ctx, uint32_t off, int width);
typedef void (*model_write_fn)(void *ctx, uint32_t off, int width, uint64_t v);
typedef struct { model_read_fn rd; model_write_fn wr; void *ctx; } page_t;

typedef struct { uint32_t regs[1024]; } regstore_t; /* 4 KiB of registers */

static uint64_t rs_read(void *ctx, uint32_t off, int width) {
  regstore_t *s = (regstore_t *)ctx;
  uint32_t w = s->regs[(off >> 2) & 1023];
  return width == 32 ? w : (w >> ((off & 3) * 8)) & ((1ull << width) - 1);
}
static void rs_write(void *ctx, uint32_t off, int width, uint64_t v) {
  regstore_t *s = (regstore_t *)ctx;
  uint32_t *r = &s->regs[(off >> 2) & 1023];
  if (width == 32) { *r = (uint32_t)v; return; }
  uint32_t sh = (off & 3) * 8, m = (uint32_t)((1ull << width) - 1) << sh;
  *r = (*r & ~m) | (((uint32_t)v << sh) & m);
}

static regstore_t stores[64];
static page_t pages[64];
static int inited;

static void init_pages(void) {
  for (int i = 0; i < 64; i++) pages[i] = (page_t){rs_read, rs_write, &stores[i]};
  inited = 1;
}

static inline int in_window(uint64_t a) {
  return (a - 0x40000000u) < 0x20000000u || (a - 0xE0000000u) < 0x100000u;
}
static inline page_t *page_of(uint64_t a) { return &pages[(a >> 23) & 63]; }

static inline uint64_t do_load(const volatile void *p, int width) {
  uint64_t a = (uint64_t)(uintptr_t)p;
  stest_now_ns += ACCESS_NS;
  if (__builtin_expect(!in_window(a), 1)) {
    switch (width) {
      case 8:  return *(const volatile uint8_t *)p;
      case 16: return *(const volatile uint16_t *)p;
      case 32: return *(const volatile uint32_t *)p;
      default: return *(const volatile uint64_t *)p;
    }
  }
  if (!inited) init_pages();
  page_t *pg = page_of(a);
  return pg->rd(pg->ctx, (uint32_t)a & 0xFFF, width);
}
static inline void do_store(volatile void *p, int width, uint64_t v) {
  uint64_t a = (uint64_t)(uintptr_t)p;
  stest_now_ns += ACCESS_NS;
  if (__builtin_expect(!in_window(a), 1)) {
    switch (width) {
      case 8:  *(volatile uint8_t *)p = (uint8_t)v; return;
      case 16: *(volatile uint16_t *)p = (uint16_t)v; return;
      case 32: *(volatile uint32_t *)p = (uint32_t)v; return;
      default: *(volatile uint64_t *)p = v; return;
    }
  }
  if (!inited) init_pages();
  page_t *pg = page_of(a);
  pg->wr(pg->ctx, (uint32_t)a & 0xFFF, width, v);
}

NOINL uint8_t  __stest_mmio_load8 (const volatile void *a) { return (uint8_t)do_load(a, 8); }
NOINL uint16_t __stest_mmio_load16(const volatile void *a) { return (uint16_t)do_load(a, 16); }
NOINL uint32_t __stest_mmio_load32(const volatile void *a) { return (uint32_t)do_load(a, 32); }
NOINL uint64_t __stest_mmio_load64(const volatile void *a) { return do_load(a, 64); }
NOINL void __stest_mmio_store8 (volatile void *a, uint8_t  v) { do_store(a, 8, v); }
NOINL void __stest_mmio_store16(volatile void *a, uint16_t v) { do_store(a, 16, v); }
NOINL void __stest_mmio_store32(volatile void *a, uint32_t v) { do_store(a, 32, v); }
NOINL void __stest_mmio_store64(volatile void *a, uint64_t v) { do_store(a, 64, v); }
